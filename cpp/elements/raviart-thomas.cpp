// Copyright (c) 2020 Chris Richardson & Matthew Scroggs
// FEniCS Project
// SPDX-License-Identifier:    MIT

#include "raviart-thomas.h"
#include "core/dof-permutations.h"
#include "core/element-families.h"
#include "core/mappings.h"
#include "core/moments.h"
#include "core/polyset.h"
#include "core/quadrature.h"
#include "lagrange.h"
#include <Eigen/Dense>
#include <numeric>
#include <vector>

using namespace basix;

//----------------------------------------------------------------------------
FiniteElement basix::create_rt(cell::type celltype, int degree)
{
  if (celltype != cell::type::triangle and celltype != cell::type::tetrahedron)
    throw std::runtime_error("Unsupported cell type");

  const int tdim = cell::topological_dimension(celltype);

  const cell::type facettype
      = (tdim == 2) ? cell::type::interval : cell::type::triangle;

  // The number of order (degree-1) scalar polynomials
  const int nv = polyset::dim(celltype, degree - 1);
  // The number of order (degree-2) scalar polynomials
  const int ns0 = polyset::dim(celltype, degree - 2);
  // The number of additional polynomials in the polynomial basis for
  // Raviart-Thomas
  const int ns = polyset::dim(facettype, degree - 1);

  // Evaluate the expansion polynomials at the quadrature points
  auto [Qpts, Qwts]
      = quadrature::make_quadrature("default", celltype, 2 * degree);
  Eigen::ArrayXXd Pkp1_at_Qpts
      = polyset::tabulate(celltype, degree, 0, Qpts)[0];

  // The number of order (degree) polynomials
  const int psize = Pkp1_at_Qpts.cols();

  // Create coefficients for order (degree-1) vector polynomials
  Eigen::MatrixXd wcoeffs = Eigen::MatrixXd::Zero(nv * tdim + ns, psize * tdim);
  for (int j = 0; j < tdim; ++j)
  {
    wcoeffs.block(nv * j, psize * j, nv, nv)
        = Eigen::MatrixXd::Identity(nv, nv);
  }

  // Create coefficients for additional polynomials in Raviart-Thomas
  // polynomial basis
  for (int i = 0; i < ns; ++i)
  {
    for (int k = 0; k < psize; ++k)
    {
      for (int j = 0; j < tdim; ++j)
      {
        const double w_sum = (Qwts * Pkp1_at_Qpts.col(ns0 + i) * Qpts.col(j)
                              * Pkp1_at_Qpts.col(k))
                                 .sum();
        wcoeffs(nv * tdim + i, k + psize * j) = w_sum;
      }
    }
  }

  // Dual space
  Eigen::MatrixXd dual = Eigen::MatrixXd::Zero(nv * tdim + ns, psize * tdim);

  // quadrature degree
  int quad_deg = 5 * degree;

  // Add rows to dualmat for integral moments on facets
  const int facet_count = tdim + 1;
  const int facet_dofs = ns;
  dual.block(0, 0, facet_count * facet_dofs, psize * tdim)
      = moments::make_normal_integral_moments(
          create_dlagrange(facettype, degree - 1), celltype, tdim, degree,
          quad_deg);

  // Add rows to dualmat for integral moments on interior
  if (degree > 1)
  {
    const int internal_dofs = tdim * ns0;
    // Interior integral moment
    dual.block(facet_count * facet_dofs, 0, internal_dofs, psize * tdim)
        = moments::make_integral_moments(create_dlagrange(celltype, degree - 2),
                                         celltype, tdim, degree, quad_deg);
  }

  const std::vector<std::vector<std::vector<int>>> topology
      = cell::topology(celltype);

  const int ndofs = dual.rows();
  int perm_count = 0;
  for (int i = 1; i < tdim; ++i)
    perm_count += topology[i].size() * i;

  std::vector<Eigen::MatrixXd> base_permutations(
      perm_count, Eigen::MatrixXd::Identity(ndofs, ndofs));
  if (tdim == 2)
  {
    Eigen::ArrayXi edge_ref = dofperms::interval_reflection(degree);
    for (int edge = 0; edge < facet_count; ++edge)
    {
      const int start = edge_ref.size() * edge;
      for (int i = 0; i < edge_ref.size(); ++i)
      {
        base_permutations[edge](start + i, start + i) = 0;
        base_permutations[edge](start + i, start + edge_ref[i]) = 1;
      }
    }

    Eigen::ArrayXXd edge_dir
        = dofperms::interval_reflection_tangent_directions(degree);
    for (int edge = 0; edge < 3; ++edge)
    {
      Eigen::MatrixXd directions = Eigen::MatrixXd::Identity(ndofs, ndofs);
      directions.block(edge_dir.rows() * edge, edge_dir.cols() * edge,
                       edge_dir.rows(), edge_dir.cols())
          = edge_dir;
      base_permutations[edge] *= directions;
    }
  }
  else if (tdim == 3)
  {
    Eigen::ArrayXi face_ref = dofperms::triangle_reflection(degree);
    Eigen::ArrayXi face_rot = dofperms::triangle_rotation(degree);

    for (int face = 0; face < facet_count; ++face)
    {
      const int start = face_ref.size() * face;
      for (int i = 0; i < face_rot.size(); ++i)
      {
        base_permutations[6 + 2 * face](start + i, start + i) = 0;
        base_permutations[6 + 2 * face](start + i, start + face_rot[i]) = 1;
        base_permutations[6 + 2 * face + 1](start + i, start + i) = 0;
        base_permutations[6 + 2 * face + 1](start + i, start + face_ref[i])
            = -1;
      }
    }
  }

  // Raviart-Thomas has ns dofs on each facet, and ns0*tdim in the interior
  std::vector<std::vector<int>> entity_dofs(topology.size());
  for (int i = 0; i < tdim - 1; ++i)
    entity_dofs[i].resize(topology[i].size(), 0);
  entity_dofs[tdim - 1].resize(topology[tdim - 1].size(), ns);
  entity_dofs[tdim] = {ns0 * tdim};

  Eigen::MatrixXd coeffs = compute_expansion_coefficients(wcoeffs, dual);
  return FiniteElement(element::family::RT, celltype, degree, {tdim}, coeffs,
                       entity_dofs, base_permutations, {}, {},
                       mapping::type::contravariantPiola);
}
//-----------------------------------------------------------------------------
Eigen::MatrixXd basix::dofperms::triangle_rt_rotation(int degree)
{
  const int n = degree * (degree + 2);
  Eigen::MatrixXd perm = Eigen::MatrixXd::Zero(n, n);

  // Permute RT functions on edges
  for (int i = 0; i < degree; ++i)
  {
    perm(i, 2 * degree + i) = 1;
    perm(2 * degree - 1 - i, i) = -1;
    perm(3 * degree - 1 - i, degree + i) = -1;
  }

  // Rotate face
  const int face_start = 3 * degree;
  Eigen::ArrayXi face_rot = dofperms::triangle_rotation(degree - 1);
  Eigen::ArrayXXd face_dir_rot
      = dofperms::triangle_rotation_tangent_directions(degree - 1);

  for (int i = 0; i < face_rot.size(); ++i)
  {
    for (int b = 0; b < 2; ++b)
      perm(face_start + i * 2 + b, face_start + face_rot[i] * 2 + b) = 1;
  }
  Eigen::MatrixXd rotation = Eigen::MatrixXd::Identity(n, n);
  rotation.block(face_start, face_start, face_dir_rot.rows(),
                 face_dir_rot.cols())
      = face_dir_rot;
  perm *= rotation;

  return perm;
}
//-----------------------------------------------------------------------------
Eigen::MatrixXd basix::dofperms::triangle_rt_reflection(int degree)
{
  const int n = degree * (degree + 2);
  Eigen::MatrixXd perm = Eigen::MatrixXd::Zero(n, n);

  // Permute RT functions on edges
  for (int i = 0; i < degree; ++i)
  {
    perm(i, degree - 1 - i) = 1;
    perm(degree + i, 2 * degree + i) = -1;
    perm(2 * degree + i, degree + i) = -1;
  }

  // reflect face
  const int face_start = 3 * degree;
  Eigen::ArrayXi face_ref = dofperms::triangle_reflection(degree - 1);
  Eigen::ArrayXXd face_dir_ref
      = dofperms::triangle_reflection_tangent_directions(degree - 1);

  for (int i = 0; i < face_ref.size(); ++i)
  {
    for (int b = 0; b < 2; ++b)
      perm(face_start + i * 2 + b, face_start + face_ref[i] * 2 + b) = 1;
  }
  Eigen::MatrixXd reflection = Eigen::MatrixXd::Identity(n, n);
  reflection.block(face_start, face_start, face_dir_ref.rows(),
                   face_dir_ref.cols())
      = face_dir_ref;
  perm *= reflection;

  return perm;
}
//-----------------------------------------------------------------------------
