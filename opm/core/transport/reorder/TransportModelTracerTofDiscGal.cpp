/*
  Copyright 2012 SINTEF ICT, Applied Mathematics.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <opm/core/transport/reorder/TransportModelTracerTofDiscGal.hpp>
#include <opm/core/grid.h>
#include <opm/core/utility/ErrorMacros.hpp>
#include <opm/core/linalg/blas_lapack.h>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace Opm
{


    /// A class providing discontinuous Galerkin basis functions.
    struct DGBasis
    {
        static int numBasisFunc(const int dimensions,
                                const int degree)
        {
            switch (dimensions) {
            case 1:
                return degree + 1;
            case 2:
                return (degree + 2)*(degree + 1)/2;
            case 3:
                return (degree + 3)*(degree + 2)*(degree + 1)/6;
            default:
                THROW("Dimensions must be 1, 2 or 3.");
            }
        }

        /// Evaluate all nonzero basis functions at x,
        /// writing to f_x. The array f_x must have
        /// size numBasisFunc(grid.dimensions, degree).
        ///
        /// The basis functions are the following
        ///     Degree 0: 1.
        ///     Degree 1: x - xc, y - yc, z - zc etc.
        /// Further degrees await development.
        static void eval(const UnstructuredGrid& grid,
                         const int cell,
                         const int degree,
                         const double* x,
                         double* f_x)
        {
            const int dim = grid.dimensions;
            const double* cc = grid.cell_centroids + dim*cell;
            // Note intentional fallthrough in this switch statement!
            switch (degree) {
            case 1:
                for (int ix = 0; ix < dim; ++ix) {
                    f_x[1 + ix] = x[ix] - cc[ix];
                }
            case 0:
                f_x[0] = 1;
                break;
            default:
                THROW("Maximum degree is 1 for now.");
            }
        }

        /// Evaluate gradients of all nonzero basis functions at x,
        /// writing to grad_f_x. The array grad_f_x must have size
        /// numBasisFunc(grid.dimensions, degree) * grid.dimensions.
        /// The <grid.dimensions> components of the first basis function
        /// gradient come before the components of the second etc.
        static void evalGrad(const UnstructuredGrid& grid,
                             const int /*cell*/,
                             const int degree,
                             const double* /*x*/,
                             double* grad_f_x)
        {
            const int dim = grid.dimensions;
            const int num_basis = numBasisFunc(dim, degree);
            std::fill(grad_f_x, grad_f_x + num_basis*dim, 0.0);
            if (degree > 1) {
                THROW("Maximum degree is 1 for now.");
            } else if (degree == 1) {
                for (int ix = 0; ix < dim; ++ix) {
                    grad_f_x[dim*(ix + 1) + ix] = 1.0;
                }
            }
        }
    };




    /// A class providing numerical quadrature for cells.
    class CellQuadrature
    {
    public:
        CellQuadrature(const UnstructuredGrid& grid,
                       const int cell,
                       const int degree)
            : grid_(grid), cell_(cell), degree_(degree)
        {
            if (degree > 1) {
                THROW("Only quadrature degree up to 1 for now.");
            }
        }

        int numQuadPts() const
        {
            return 1;
        }

        void quadPtCoord(int /*index*/, double* coord) const
        {
            const double* cc = grid_.cell_centroids + grid_.dimensions*cell_;
            std::copy(cc, cc + grid_.dimensions, coord);
        }

        double quadPtWeight(int /*index*/) const
        {
            return 1.0;
        }

    private:
        const UnstructuredGrid& grid_;
        const int cell_;
        const int degree_;
    };



    /// A class providing numerical quadrature for faces.
    class FaceQuadrature
    {
    public:
        FaceQuadrature(const UnstructuredGrid& grid,
                       const int face,
                       const int degree)
            : grid_(grid), face_(face), degree_(degree)
        {
            if (degree > 1) {
                THROW("Only quadrature degree up to 1 for now.");
            }
        }

        int numQuadPts() const
        {
            return 1;
        }

        void quadPtCoord(int /*index*/, double* coord) const
        {
            const double* fc = grid_.face_centroids + grid_.dimensions*face_;
            std::copy(fc, fc + grid_.dimensions, coord);
        }

        double quadPtWeight(int /*index*/) const
        {
            return 1.0;
        }

    private:
        const UnstructuredGrid& grid_;
        const int face_;
        const int degree_;
    };


    // Initial version: only a constant interpolation.
    static void interpolateVelocity(const UnstructuredGrid& grid,
                                    const int cell,
                                    const double* darcyflux,
                                    const double* /*x*/,
                                    double* v)
    {
        const int dim = grid.dimensions;
        std::fill(v, v + dim, 0.0);
        const double* cc = grid.cell_centroids + cell*dim;
        for (int hface = grid.cell_facepos[cell]; hface < grid.cell_facepos[cell+1]; ++hface) {
            const int face = grid.cell_faces[hface];
            const double* fc = grid.face_centroids + face*dim;
            double flux = 0.0;
            if (cell == grid.face_cells[2*face]) {
                flux = darcyflux[face];
            } else {
                flux = -darcyflux[face];
            }
            for (int dd = 0; dd < dim; ++dd) {
                v[dd] += flux * (fc[dd] - cc[dd]) / grid.cell_volumes[cell];
            }
        }
    }




    /// Construct solver.
    /// \param[in] grid      A 2d or 3d grid.
    TransportModelTracerTofDiscGal::TransportModelTracerTofDiscGal(const UnstructuredGrid& grid)
        : grid_(grid),
          coord_(grid.dimensions),
          velocity_(grid.dimensions)
    {
    }




    /// Solve for time-of-flight at next timestep.
    /// \param[in]  darcyflux         Array of signed face fluxes.
    /// \param[in]  porevolume        Array of pore volumes.
    /// \param[in]  source            Transport source term.
    /// \param[in]  degree            Polynomial degree of DG basis functions used.
    /// \param[out] tof_coeff         Array of time-of-flight solution coefficients.
    ///                               The values are ordered by cell, meaning that
    ///                               the K coefficients corresponding to the first
    ///                               cell comes before the K coefficients corresponding
    ///                               to the second cell etc.
    ///                               K depends on degree and grid dimension.
    void TransportModelTracerTofDiscGal::solveTof(const double* darcyflux,
                                                  const double* porevolume,
                                                  const double* source,
                                                  const int degree,
                                                  std::vector<double>& tof_coeff)
    {
        darcyflux_ = darcyflux;
        porevolume_ = porevolume;
        source_ = source;
#ifndef NDEBUG
        // Sanity check for sources.
        const double cum_src = std::accumulate(source, source + grid_.number_of_cells, 0.0);
        if (std::fabs(cum_src) > *std::max_element(source, source + grid_.number_of_cells)*1e-2) {
            THROW("Sources do not sum to zero: " << cum_src);
        }
#endif
        degree_ = degree;
        const int num_basis = DGBasis::numBasisFunc(grid_.dimensions, degree_);
        tof_coeff.resize(num_basis*grid_.number_of_cells);
        std::fill(tof_coeff.begin(), tof_coeff.end(), 0.0);
        tof_coeff_ = &tof_coeff[0];
        rhs_.resize(num_basis);
        jac_.resize(num_basis*num_basis);
        basis_.resize(num_basis);
        basis_nb_.resize(num_basis);
        grad_basis_.resize(num_basis*grid_.dimensions);
        reorderAndTransport(grid_, darcyflux);
    }




    void TransportModelTracerTofDiscGal::solveSingleCell(const int cell)
    {
        // Residual:
        // For each cell K, basis function b_j (spanning V_h),
        // writing the solution u_h|K = \sum_i c_i b_i
        //  Res = - \int_K \sum_i c_i b_i v(x) \cdot \grad b_j dx
        //        + \int_{\partial K} F(u_h, u_h^{ext}, v(x) \cdot n) b_j ds
        //        - \int_K \phi b_j
        // This is linear in c_i, so we do not need any nonlinear iterations.
        // We assemble the jacobian and the right-hand side. The residual is
        // equal to Res = Jac*c - rhs, and we compute rhs directly.

        const int dim = grid_.dimensions;
        const int num_basis = DGBasis::numBasisFunc(degree_, dim);

        std::fill(rhs_.begin(), rhs_.end(), 0.0);
        std::fill(jac_.begin(), jac_.end(), 0.0);

        // Compute cell residual contribution.
        // Note: Assumes that \int_K b_j = 0 for all j > 0
        rhs_[0] += porevolume_[cell];

        // Compute upstream residual contribution.
        for (int hface = grid_.cell_facepos[cell]; hface < grid_.cell_facepos[cell+1]; ++hface) {
            const int face = grid_.cell_faces[hface];
            double flux = 0.0;
            int upstream_cell = -1;
            if (cell == grid_.face_cells[2*face]) {
                flux = darcyflux_[face];
                upstream_cell = grid_.face_cells[2*face+1];
            } else {
                flux = -darcyflux_[face];
                upstream_cell = grid_.face_cells[2*face];
            }
            if (upstream_cell < 0) {
                // This is an outer boundary. Assumed tof = 0 on inflow, so no contribution.
                continue;
            }
            if (flux >= 0.0) {
                // This is an outflow boundary.
                continue;
            }
            // Do quadrature over the face to compute
            // \int_{\partial K} u_h^{ext} (v(x) \cdot n) b_j ds
            // (where u_h^{ext} is the upstream unknown (tof)).
            FaceQuadrature quad(grid_, face, degree_);
            for (int quad_pt = 0; quad_pt < quad.numQuadPts(); ++quad_pt) {
                // u^ext flux B   (B = {b_j})
                quad.quadPtCoord(quad_pt, &coord_[0]);
                DGBasis::eval(grid_, cell, degree_, &coord_[0], &basis_[0]);
                DGBasis::eval(grid_, upstream_cell, degree_, &coord_[0], &basis_nb_[0]);
                const double tof_upstream = std::inner_product(basis_nb_.begin(), basis_nb_.end(),
                                                               tof_coeff_ + num_basis*upstream_cell, 0.0);
                const double w = quad.quadPtWeight(quad_pt);
                for (int j = 0; j < num_basis; ++j) {
                    rhs_[j] -= w * tof_upstream * flux * basis_[j];
                }
            }
        }

        // Compute cell jacobian contribution. We use Fortran ordering
        // for jac_, i.e. rows cycling fastest.
        {
            CellQuadrature quad(grid_, cell, degree_);
            for (int quad_pt = 0; quad_pt < quad.numQuadPts(); ++quad_pt) {
                // b_i (v \cdot \grad b_j)
                quad.quadPtCoord(quad_pt, &coord_[0]);
                DGBasis::eval(grid_, cell, degree_, &coord_[0], &basis_[0]);
                DGBasis::evalGrad(grid_, cell, degree_, &coord_[0], &grad_basis_[0]);
                interpolateVelocity(grid_, cell, darcyflux_, &coord_[0], &velocity_[0]);
                const double w = quad.quadPtWeight(quad_pt);
                for (int j = 0; j < num_basis; ++j) {
                    for (int i = 0; i < num_basis; ++i) {
                        for (int dd = 0; dd < dim; ++dd) {
                            jac_[j*num_basis + i] += w * basis_[i] * grad_basis_[dim*j + dd] * velocity_[dd];
                        }
                    }
                }
            }
        }

        // Compute downstream jacobian contribution.
        for (int hface = grid_.cell_facepos[cell]; hface < grid_.cell_facepos[cell+1]; ++hface) {
            const int face = grid_.cell_faces[hface];
            double flux = 0.0;
            if (cell == grid_.face_cells[2*face]) {
                flux = darcyflux_[face];
            } else {
                flux = -darcyflux_[face];
            }
            if (flux <= 0.0) {
                // This is an inflow boundary.
                continue;
            }
            // Do quadrature over the face to compute
            // \int_{\partial K} b_i (v(x) \cdot n) b_j ds
            FaceQuadrature quad(grid_, face, degree_);
            for (int quad_pt = 0; quad_pt < quad.numQuadPts(); ++quad_pt) {
                // u^ext flux B   (B = {b_j})
                quad.quadPtCoord(quad_pt, &coord_[0]);
                DGBasis::eval(grid_, cell, degree_, &coord_[0], &basis_[0]);
                const double w = quad.quadPtWeight(quad_pt);
                for (int j = 0; j < num_basis; ++j) {
                    for (int i = 0; i < num_basis; ++i) {
                        jac_[j*num_basis + i] += w * basis_[i] * flux * basis_[j];
                    }
                }
            }
        }

        // Solve linear equation.
        MAT_SIZE_T n = num_basis;
        MAT_SIZE_T nrhs = 1;
        MAT_SIZE_T lda = num_basis;
        std::vector<MAT_SIZE_T> piv(num_basis);
        MAT_SIZE_T ldb = num_basis;
        MAT_SIZE_T info = 0;
        dgesv_(&n, &nrhs, &jac_[0], &lda, &piv[0], &rhs_[0], &ldb, &info);
        if (info != 0) {
            THROW("Lapack error: " << info);
        }
        // The solution ends up in rhs_, so we must copy it.
        std::copy(rhs_.begin(), rhs_.end(), tof_coeff_ + num_basis*cell);
    }




    void TransportModelTracerTofDiscGal::solveMultiCell(const int num_cells, const int* cells)
    {
        std::cout << "Pretending to solve multi-cell dependent equation with " << num_cells << " cells." << std::endl;
        for (int i = 0; i < num_cells; ++i) {
            solveSingleCell(cells[i]);
        }
    }




} // namespace Opm
