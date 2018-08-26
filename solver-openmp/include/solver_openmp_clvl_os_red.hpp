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

#ifndef MBSOLVE_SOLVER_OPENMP_3LVL_OS_RED_H
#define MBSOLVE_SOLVER_OPENMP_3LVL_OS_RED_H

#include <iostream>
#include <omp.h>
#include <solver.hpp>
#include <internal/common_fdtd_2lvl.hpp>
#include <internal/copy_list_entry.hpp>

namespace mbsolve {

template<unsigned int num_lvl>
class sim_constants_clvl_os
{
    static const unsigned int num_adj = num_lvl * num_lvl - 1;

    typedef Eigen::Matrix<complex, num_adj, num_adj> complex_matrix_t;
    typedef Eigen::Matrix<real, num_adj, num_adj> real_matrix_t;
    typedef Eigen::Matrix<real, num_adj, 1> real_vector_t;

public:
    /* constant propagators */
    //    complex_matrix_t B_1;
    //    complex_matrix_t B_2;

    bool has_qm;
    bool has_dipole;

    /* analytic solution precalc */
    real_matrix_t coeff_1[num_adj/2];
    real_matrix_t coeff_2[num_adj/2];
    real theta[num_adj/2];

    /* rodrigues formula precalc */
    real_matrix_t U2;
    real theta_1;

    /* constant propagator A_0 = exp(M dt/2) */
    real_matrix_t A_0;

    /* unitary transformation matrix */
    complex_matrix_t B;

    /* required for polarization calc ? */
    real_matrix_t M;
    real_matrix_t U;
    real_vector_t d_in;
    real_vector_t d_eq;

    /* dipole moments */
    real_vector_t v;

    /* diagonalized interaction propagator */
    /* TODO: special type for diagonal matrix? */
    /* TODO: vector would do, right? */
    Eigen::Matrix<complex, num_adj, 1> L;

    /* electromagnetic constants */
    real M_CE;
    real M_CH;
    real M_CP;
    real sigma;

    /* simulation constants */
    real d_x_inv;
    real d_t;

    /* initialization constants */
    real_vector_t d_init;

};

template<unsigned int num_lvl>
class solver_openmp_clvl_os_red : public solver_int
{
    static const unsigned int num_adj = num_lvl * num_lvl - 1;

    typedef Eigen::Matrix<complex, num_adj, num_adj> complex_matrix_t;
    typedef Eigen::Matrix<real, num_adj, num_adj> real_matrix_t;
    typedef Eigen::Matrix<real, num_adj, 1> real_vector_t;

public:
    solver_openmp_clvl_os_red(std::shared_ptr<const device> dev,
                              std::shared_ptr<scenario> scen);

    ~solver_openmp_clvl_os_red();

    const std::string& get_name() const;

    void run() const;

private:
    const std::string m_name;

    /* TODO: rule of three. make copy constructor etc. private?
     * or implement correctly
     */

    /*
     * Position-dependent density matrix in adjoint representation.
     */
    real_vector_t **m_d;

    real **m_h;
    real **m_e;
    real **m_p;

    real *m_result_scratch;

    uint64_t m_scratch_size;

    real *m_source_data;

    unsigned int **m_mat_indices;

#ifdef XEON_PHI_OFFLOAD
    copy_list_entry_dev *l_copy_list;
#else
    copy_list_entry *l_copy_list;
#endif
    sim_source *l_sim_sources;
    sim_constants_clvl_os<num_lvl> *l_sim_consts;

    std::vector<sim_constants_clvl_os<num_lvl> > m_sim_consts;

    std::vector<sim_source> m_sim_sources;

    std::vector<copy_list_entry> m_copy_list;

};

typedef solver_openmp_clvl_os_red<3> solver_openmp_3lvl_os_red;

}

#endif
