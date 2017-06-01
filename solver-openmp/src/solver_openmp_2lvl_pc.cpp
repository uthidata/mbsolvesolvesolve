/*
 * mbsolve: Framework for solving the Maxwell-Bloch/-Lioville equations
 *
 * Copyright (c) 2016, Computational Photonics Group, Technical University of
 * Munich.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <cmath>
#include <iostream>
#include <omp.h>
#include <solver_openmp_2lvl_pc.hpp>

namespace mbsolve {

static solver_factory<solver_openmp_2lvl_pc> factory("openmp-2lvl-pc");

/* TODO: necessary? */
unsigned int num_grid_points;
unsigned int num_time_steps;
real time_step_size;
#if 0
    num_grid_points = m_scenario.NumGridPoints;
    num_time_steps = m_scenario.NumTimeSteps;
    time_step_size = m_scenario.TimeStepSize;
#endif


solver_openmp_2lvl_pc::solver_openmp_2lvl_pc(std::shared_ptr<const device> dev,
                                             std::shared_ptr<scenario> scen) :
solver_int(dev, scen), m_sim_consts(dev->get_used_materials().size())
{
    /* TODO: scenario, device sanity check */
    /*
     * device.length > 0 (-> regions.size() > 0)
     * required materials found?
     * no gap in regions
     *
     */

    if (dev->get_regions().size() == 0) {
        throw std::invalid_argument("No regions in device!");
    }

    std::cout << "Reg3: " << dev->get_regions()[2]->get_name() << std::endl;


    /* determine simulation settings */
    if (scen->get_num_gridpoints() > 0) {
        /* courant number */
        /* TODO: simulation parameter? */
        real C = 0.5;

        /* speed of light (use smallest value of relative permittivities) */
        real velocity = 1.0/sqrt(MU0 * EPS0 * dev->get_minimum_permittivity());

        /* get number of grid points */
        unsigned int n_x = scen->get_num_gridpoints();

        /* grid point size */
        real d_x = dev->get_length()/(n_x - 1);
        scen->set_gridpoint_size(d_x);

        /* time step size */
        real d_t = C * d_x/velocity;

        /* number of time steps */
        unsigned int n_t = ceil(scen->get_endtime()/d_t) + 1;
        scen->set_num_timesteps(n_t);

        /* re-adjust time step size in order to fit number of time steps */
        d_t = scen->get_endtime()/(n_t - 1);
        scen->set_timestep_size(d_t);

    } else {
        throw std::invalid_argument("Invalid scenario.");
    }


    std::cout << "Used materials: " << dev->get_used_materials().size() << std::endl;

    /* set up simulaton constants */
    unsigned int j = 0;
    std::map<std::string, unsigned int> id_to_idx;
    for (auto mat_id : dev->get_used_materials()) {
        struct sim_constants_2lvl sc;

        auto mat = material::get_from_library(mat_id);

        /* factor for electric field update */
        sc.M_CE = scen->get_timestep_size()/
            (EPS0 * mat->get_rel_permittivity());

        /* factor for magnetic field update */
        sc.M_CH = scen->get_timestep_size()/
            (MU0 * mat->get_rel_permeability() * scen->get_gridpoint_size());

        /* convert loss term to conductivity */
        sc.sigma = sqrt(EPS0 * mat->get_rel_permittivity()/
                        (MU0 * mat->get_rel_permeability()))
            * mat->get_losses() * 2.0;

        /* active region in 2-lvl description? */
        /* TODO: remove ugly dynamic cast to qm_desc_2lvl */
        std::shared_ptr<qm_desc_2lvl> qm =
            std::dynamic_pointer_cast<qm_desc_2lvl>(mat->get_qm());
        if (qm) {
            /* factor for macroscopic polarization */
            sc.M_CP = -2.0 * HBAR * mat->get_overlap_factor() *
                qm->get_carrier_density();

            /* 2-lvl quantum mechanical system */
            sc.w12 = qm->get_transition_freq();
            sc.d12 = qm->get_dipole_moment() * E0/HBAR;
            sc.tau1 = qm->get_scattering_rate();
            sc.gamma12 = qm->get_dephasing_rate();

            /* TODO: evaluate flexible initialization in scenario */
            sc.dm11_init = 0.0;
            sc.dm22_init = 1.0;

        } else {
            /* set all qm-related factors to zero */
            sc.M_CP = 0.0;
            sc.w12 = 0.0;
            sc.d12 = 0.0;
            sc.tau1 = 0.0;
            sc.gamma12 = 0.0;

            /* initialization */
            sc.dm11_init = 0.0;
            sc.dm22_init = 0.0;
        }

        /* simulation settings */
        sc.d_x_inv = 1.0/scen->get_gridpoint_size();
        sc.d_t = scen->get_timestep_size();

        m_sim_consts.push_back(sc);
        id_to_idx[mat->get_id()] = j;
        j++;
    }

    /* allocate data arrays */
    m_dm11 = new real[scen->get_num_gridpoints()];
    m_dm12r = new real[scen->get_num_gridpoints()];
    m_dm12i = new real[scen->get_num_gridpoints()];
    m_dm22 = new real[scen->get_num_gridpoints()];
    m_h = new real[scen->get_num_gridpoints() + 1];
    m_e = new real[scen->get_num_gridpoints()];
    m_mat_indices = new unsigned int[scen->get_num_gridpoints()];

    /* set up indices array and initialize data arrays */
#pragma omp for schedule(static)
    for (unsigned int i = 0; i < scen->get_num_gridpoints(); i++) {
        int idx = -1;



        /* determine material of index */
        real x = i * scen->get_gridpoint_size();
        for (auto reg : dev->get_regions()) {
            if ((x >= reg->get_start()) && (x <= reg->get_end())) {
                idx = id_to_idx[reg->get_material()->get_id()];
                break;
            }
        }
        /* TODO: assert/bug if idx == -1 */
        if (idx < 0) {
            std::cout << "At index " << i << std::endl;
            throw std::invalid_argument("region not found");
        }
        m_mat_indices[i] = idx;

        /* TODO: evaluate flexible initialization in scenario */
        m_dm11[i] = m_sim_consts[idx].dm11_init;
        m_dm22[i] = m_sim_consts[idx].dm22_init;
        m_dm12r[i] = 0.0;
        m_dm12i[i] = 0.0;
        m_e[i] = 0.0;
        m_h[i] = 0.0;
        if (i == scen->get_num_gridpoints() - 1) {
            m_h[i + 1] = 0.0;
        }
    }

    /* set up results and transfer data structures */
    unsigned int scratch_size = 0;
    for (auto rec : scen->get_records()) {
        /* create copy list entry */
        copy_list_entry entry(rec, scen);

        /* add result to solver */
        m_results.push_back(entry.get_result());

        /* calculate scratch size */
        scratch_size += entry.get_size();

        /* TODO: make more generic? */
        /* TODO: move to parser in record class */
        /* add source address to copy list entry */
        if (rec->get_name() == "d11") {
            entry.set_real(m_dm11);
        } else if (rec->get_name() == "d22") {
            entry.set_real(m_dm22);
        } else if (rec->get_name() == "d12") {
            entry.set_real(m_dm12r);
            entry.set_imag(m_dm12i);

            /* take imaginary part into account */
            scratch_size += entry.get_size();
        } else if (rec->get_name() == "e") {
            entry.set_real(m_e);
        } else if (rec->get_name() == "h") {
            /* TODO: numGridPoints + 1 ? */
            entry.set_real(m_h);
        } else {
            throw std::invalid_argument("Requested result is not available!");
        }

        m_copy_list.push_back(entry);
    }

    /* allocate scratchpad result memory */
    m_result_scratch = new real[scratch_size];

    std::cout << "Scratch size " << scratch_size << std::endl;

    /* add scratchpad addresses to copy list entries */
    unsigned int scratch_offset = 0;
    for (auto& cle : m_copy_list) {
        cle.set_scratch_real(&m_result_scratch[scratch_offset]);
        scratch_offset += cle.get_size();

        if (cle.get_record()->get_name() == "d12") {
            /* complex result */
            cle.set_scratch_imag(&m_result_scratch[scratch_offset]);
            scratch_offset += cle.get_size();
        }
    }
}

solver_openmp_2lvl_pc::~solver_openmp_2lvl_pc()
{
    delete[] m_h;
    delete[] m_e;
    delete[] m_dm11;
    delete[] m_dm12r;
    delete[] m_dm12i;
    delete[] m_dm22;
    delete[] m_mat_indices;
    delete[] m_result_scratch;
}

const std::string&
solver_openmp_2lvl_pc::get_name() const
{
    return factory.get_name();
}

void
solver_openmp_2lvl_pc::run() const
{
    /*
#pragma offload target(mic) in(gsc, num_grid_points, num_time_steps) \
  in(time_step_size) \
  inout(m_e:length(m_scenario.NumGridPoints)) \
  inout(m_h:length(m_scenario.NumGridPoints + 1)) \
  inout(m_dm11:length(m_scenario.NumGridPoints)) \
  inout(m_dm12r:length(m_scenario.NumGridPoints)) \
  inout(m_dm12i:length(m_scenario.NumGridPoints)) \
  inout(m_dm22:length(m_scenario.NumGridPoints)) \
  in(region_indices:length(m_scenario.NumGridPoints))
  {
    */
#pragma omp parallel
    {
          /* main loop */
          for (int n = 0; n < m_scenario->get_num_timesteps(); n++) {
              /* calculate source value */
              /* TODO optimize divisions? */
              /* TODO move to source? */
              real f_0 = 2e14;
              real t = n * m_scenario->get_timestep_size();
              real T_p = 20/f_0;
              real gamma = 2 * t/T_p - 1;
              real E_0 = 4.2186e9;
              real src = E_0 * 1/std::cosh(10 * gamma)
                  * sin(2 * M_PI * f_0 * t);
              //src = src/2; // pi pulse
              /* TODO:  private(src) ? */
              /* TODO: masteronly? */

              /* update dm and e in parallel */
              //#pragma omp for simd schedule(static)
#pragma omp for schedule(static)
              for (int i = 0; i < m_scenario->get_num_gridpoints(); i++) {
                  unsigned int mat_idx = m_mat_indices[i];

                  real rho11_e = m_dm11[i];
                  real rho12r_e = m_dm12r[i];
                  real rho12i_e = m_dm12i[i];
                  real rho22_e = m_dm22[i];
                  real field_e = m_e[i];

                  for (int pc_step = 0; pc_step < 4; pc_step++) {
                      /* execute prediction - correction steps */

                      real rho11  = 0.5 * (m_dm11[i] + rho11_e);
                      real rho22  = 0.5 * (m_dm22[i] + rho22_e);
                      real rho12r = 0.5 * (m_dm12r[i] + rho12r_e);
                      real rho12i = 0.5 * (m_dm12i[i] + rho12i_e);
                      real OmRabi = 0.5 * m_sim_consts[mat_idx].d12
                          * (m_e[i] + field_e);

                      rho11_e = m_dm11[i] + m_sim_consts[mat_idx].d_t *
                          (- 2.0 * OmRabi * rho12i
                           - m_sim_consts[mat_idx].tau1 * rho11);

                      rho12i_e = m_dm12i[i] + m_sim_consts[mat_idx].d_t *
                          (- m_sim_consts[mat_idx].w12 * rho12r
                           + OmRabi * (rho11 - rho22)
                           - m_sim_consts[mat_idx].gamma12 * rho12i);

                      rho12r_e = m_dm12r[i] + m_sim_consts[mat_idx].d_t *
                          (+ m_sim_consts[mat_idx].w12 * rho12i
                           - m_sim_consts[mat_idx].gamma12 * rho12r);

                      rho22_e = m_dm22[i] + m_sim_consts[mat_idx].d_t *
                          (+ 2.0 * OmRabi * rho12i
                           + m_sim_consts[mat_idx].tau1 * rho11);

                      real j = 0;
                      real p_t = m_sim_consts[mat_idx].M_CP
                          * m_sim_consts[mat_idx].d12 *
                          (m_sim_consts[mat_idx].w12 * rho12i -
                           m_sim_consts[mat_idx].gamma12 * rho12r);

                      field_e = m_e[i] + m_sim_consts[mat_idx].M_CE *
                          (-j - p_t + (m_h[i + 1] - m_h[i])
                           * m_sim_consts[mat_idx].d_x_inv);

                      if (i == 0) {
                          /* TODO rework soft source for pred-corr scheme */
                          //m_e_est[i] += src; /* soft source */
                          field_e = src; /* hard source */
                      }
                  }

                  /* final update step */
                  m_dm11[i] = rho11_e;
                  m_dm12i[i] = rho12i_e;
                  m_dm12r[i] = rho12r_e;
                  m_dm22[i] = rho22_e;

                  m_e[i] = field_e;
              }

              /* update h in parallel */
              //#pragma omp for simd schedule(static)
#pragma omp for schedule(static)
              for (int i = 1; i < m_scenario->get_num_gridpoints(); i++) {
                  unsigned int mat_idx = m_mat_indices[i - 1];

                  m_h[i] += m_sim_consts[mat_idx].M_CH * (m_e[i] - m_e[i - 1]);
              }

              /* save results to scratchpad in parallel */
              for (const auto& cle : m_copy_list) {
                  if (cle.hasto_record(n)) {
#pragma omp for schedule(static)
                      for (int i = 0; i < m_scenario->get_num_gridpoints();
                           i++) {
                          if ((i > cle.get_position()) &&
                              (i < cle.get_position() + cle.get_cols())) {
                              *cle.get_scratch_real(n, i) = *cle.get_real(i);
                              if (cle.is_complex()) {
                                  *cle.get_scratch_imag(n, i) =
                                      *cle.get_imag(i);
                              }
                          }
                      }
                  }
              }
          }
    }

    /* bulk copy results into result classes */
    for (const auto& cle : m_copy_list) {
        std::copy(cle.get_scratch_real(0, 0), cle.get_scratch_real(0, 0) +
                  cle.get_size(), cle.get_result_real(0, 0));
        if (cle.is_complex()) {
            std::copy(cle.get_scratch_imag(0, 0), cle.get_scratch_imag(0, 0) +
                      cle.get_size(), cle.get_result_imag(0, 0));
        }
    }
}

}
