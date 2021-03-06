/*
 *                This source code is part of
 *
 *                     E  R  K  A  L  E
 *                             -
 *                       HF/DFT from Hel
 *
 * Written by Susi Lehtola, 2010-2011
 * Copyright (c) 2010-2011, Susi Lehtola
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef ERKALE_POPULATION
#define ERKALE_POPULATION

#include "global.h"
#include <armadillo>
class BasisSet;

/// Add in the nuclear charges to q
arma::vec add_nuclear_charges(const BasisSet & basis, const arma::vec & q);

/// Compute Mulliken charges
arma::vec mulliken_charges(const BasisSet & basis, const arma::mat & P);
/// Compute Mulliken charges
arma::mat mulliken_charges(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb);

/// Mulliken population analysis
void mulliken_analysis(const BasisSet & basis, const arma::mat & P);
/// Mulliken population analysis
void mulliken_analysis(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb);

/// Compute Löwdin charges
arma::vec lowdin_charges(const BasisSet & basis, const arma::mat & P);
/// Compute Löwdin charges
arma::mat lowdin_charges(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb);

/// Löwdin population analysis
void lowdin_analysis(const BasisSet & basis, const arma::mat & P);
/// Löwdin population analysis
void lowdin_analysis(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb);

/// Compute IAO charges
arma::vec IAO_charges(const BasisSet & basis, const arma::mat & C, std::string minbas="MINAO.gbs");
/// Compute IAO charges, complex coefficients
arma::vec IAO_charges(const BasisSet & basis, const arma::cx_mat & C, std::string minbas="MINAO.gbs");

/// IAO population analysis
void IAO_analysis(const BasisSet & basis, const arma::mat & C, std::string minbas="MINAO.gbs");
/// IAO population analysis
void IAO_analysis(const BasisSet & basis, const arma::cx_mat & C, std::string minbas="MINAO.gbs");
/// IAO population analysis
void IAO_analysis(const BasisSet & basis, const arma::mat & Ca, const arma::mat & Cb, std::string minbas="MINAO.gbs");
/// IAO population analysis
void IAO_analysis(const BasisSet & basis, const arma::cx_mat & Ca, const arma::cx_mat & Cb, std::string minbas="MINAO.gbs");

/// Compute electron density at nuclei
arma::vec nuclear_density(const BasisSet & basis, const arma::mat & P);

/// Compute nuclear density analysis
void nuclear_analysis(const BasisSet & basis, const arma::mat & P);
/// Compute nuclear density analysis
void nuclear_analysis(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb);

/// Compute Becke charges
arma::vec becke_charges(const BasisSet & basis, const arma::mat & P, double tol=1e-5);
/// Compute Becke charges
arma::mat becke_charges(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb, double tol=1e-5);

/// Compute Becke analysis
void becke_analysis(const BasisSet & basis, const arma::mat & P, double tol=1e-5);
/// Compute Becke analysis
void becke_analysis(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb, double tol=1e-5);

/// Compute Hirshfeld charges
arma::vec hirshfeld_charges(const BasisSet & basis, const arma::mat & P, std::string method="HF", double tol=1e-5);
/// Compute Hirshfeld charges
arma::mat hirshfeld_charges(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb, std::string method="HF", double tol=1e-5);

/// Compute Hirshfeld analysis
void hirshfeld_analysis(const BasisSet & basis, const arma::mat & P, std::string method="HF", double tol=1e-5);
/// Compute Hirshfeld analysis
void hirshfeld_analysis(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb, std::string method="HF", double tol=1e-5);

/// Compute iterative Hirshfeld charges
arma::vec iterative_hirshfeld_charges(const BasisSet & basis, const arma::mat & P, std::string method="HF", double tol=1e-5);
/// Compute iterative Hirshfeld charges
arma::mat iterative_hirshfeld_charges(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb, std::string method="HF", double tol=1e-5);

/// Compute iterative Hirshfeld analysis
void iterative_hirshfeld_analysis(const BasisSet & basis, const arma::mat & P, std::string method="HF", double tol=1e-5);
/// Compute iterative Hirshfeld analysis
void iterative_hirshfeld_analysis(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb, std::string method="HF", double tol=1e-5);

/// Compute Stockholder charges
arma::vec stockholder_charges(const BasisSet & basis, const arma::mat & P, double tol=1e-5);
/// Compute Stockholder charges
arma::mat stockholder_charges(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb, double tol=1e-5);

/// Compute Stockholder analysis
void stockholder_analysis(const BasisSet & basis, const arma::mat & P, double tol=1e-5);
/// Compute Stockholder analysis
void stockholder_analysis(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb, double tol=1e-5);

/// Compute Bader charges
arma::vec bader_charges(const BasisSet & basis, const arma::mat & P, double tol=1e-5);
/// Compute Bader charges
arma::mat bader_charges(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb, double tol=1e-5);

/// Compute Bader analysis
void bader_analysis(const BasisSet & basis, const arma::mat & P, double tol=1e-5);
/// Compute Bader analysis
void bader_analysis(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb, double tol=1e-5);

/// Compute Voronoi charges
arma::vec voronoi_charges(const BasisSet & basis, const arma::mat & P, double tol=1e-5);
/// Compute Voronoi charges
arma::mat voronoi_charges(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb, double tol=1e-5);

/// Compute Voronoi analysis
void voronoi_analysis(const BasisSet & basis, const arma::mat & P, double tol=1e-5);
/// Compute Voronoi analysis
void voronoi_analysis(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb, double tol=1e-5);


/**
 * The Mulliken population analysis stuff is based on the book "Simple
 * theorems, proofs, and derivations in Quantum Chemistry" by István
 * Mayer (IM), Kluwer Academic 2003.
 */

/**
 * Computes Mulliken's overlap population
 * \f$ d_{AB} = \sum_{\mu \in A} \sum_{\nu in B} P_{\mu \nu} S_{\mu \nu} \f$
 * (IM) eqn 7.17
 */
arma::mat mulliken_overlap(const BasisSet & basis, const arma::mat & P);

/**
 * Compute bond order index
 * \f$ B_{AB} = \sum_{\mu \in A} \sum_{\nu in B} ({\mathbf PS})_{\mu \nu} \f$
 * (IM) eqn 7.35
 */
arma::mat bond_order(const BasisSet & basis, const arma::mat & P);

/**
 * Compute bond order index for open-shell case
 * (IM) eqn 7.36
 */
arma::mat bond_order(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb);

/**
 * Default set of population analysis
 */
void population_analysis(const BasisSet & basis, const arma::mat & P);
void population_analysis(const BasisSet & basis, const arma::mat & Pa, const arma::mat & Pb);

/// Calculate S^2 spin expectation value, given sets of occupied orbitals
double spin_S2(const BasisSet & basis, const arma::mat & Ca, const arma::mat & Cb);

/**
 * Get Darwin one-electron term.
 */
double darwin_1e(const BasisSet & basis, const arma::mat & P);

/**
 * Get mass-velocity term.
 */
double mass_velocity(const BasisSet & basis, const arma::mat & P);

#endif
