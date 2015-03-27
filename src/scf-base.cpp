/*
 *                This source code is part of
 *
 *                     E  R  K  A  L  E
 *                             -
 *                       DFT from Hel
 *
 * Written by Susi Lehtola, 2010-2011
 * Copyright (c) 2010-2011, Susi Lehtola
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */



#include <armadillo>
#include <cstdio>
#include <cfloat>

#include "basis.h"
#include "broyden.h"
#include "checkpoint.h"
#include "elements.h"
#include "dftfuncs.h"
#include "dftgrid.h"
#include "global.h"
#include "guess.h"
#include "lebedev.h"
#include "linalg.h"
#include "mathf.h"
#include "properties.h"
#include "scf.h"
#include "settings.h"
#include "stringutil.h"
#include "timer.h"
#include "trrh.h"
#include "localization.h"
#include "pzstability.h"

extern "C" {
#include <gsl/gsl_poly.h>
}

enum guess_t parse_guess(const std::string & val) {
  if(stricmp(val,"Core")==0)
    return COREGUESS;
  else if(stricmp(val,"GWH")==0)
    return GWHGUESS;
  else if(stricmp(val,"Atomic")==0)
    return ATOMGUESS;
  else if(stricmp(val,"Molecular")==0)
    return MOLGUESS;
  else
    throw std::runtime_error("Guess type not supported.\n");
}

SCF::SCF(const BasisSet & basis, const Settings & set, Checkpoint & chkpt) {
  // Amount of basis functions
  Nbf=basis.get_Nbf();

  basisp=&basis;
  chkptp=&chkpt;

  // Multiplicity
  mult=set.get_int("Multiplicity");

  // Amount of electrons
  Nel=basis.Ztot()-set.get_int("Charge");

  // Parse guess
  guess=parse_guess(set.get_string("Guess"));

  usediis=set.get_bool("UseDIIS");
  diis_c1=set.get_bool("C1-DIIS");
  diisorder=set.get_int("DIISOrder");
  diiseps=set.get_double("DIISEps");
  diisthr=set.get_double("DIISThr");
  useadiis=set.get_bool("UseADIIS");
  usebroyden=set.get_bool("UseBroyden");
  usetrrh=set.get_bool("UseTRRH");
  linesearch=set.get_bool("LineSearch");

  realcmos=false;

  maxiter=set.get_int("MaxIter");
  shift=set.get_double("Shift");
  verbose=set.get_bool("Verbose");

  direct=set.get_bool("Direct");
  decfock=set.get_bool("DecFock");
  strictint=set.get_bool("StrictIntegrals");
  // Integral screening threshold
  intthr=strictint ? DBL_EPSILON : set.get_double("IntegralThresh");
  // Sanity check
  if(intthr>1e-6) {
    fprintf(stderr,"Warning - spuriously large integral threshold %e\n",intthr);
  }

  doforce=false;

  // Check update scheme
  if(useadiis && usebroyden) {
    ERROR_INFO();
    throw std::runtime_error("ADIIS and Broyden mixing cannot be used at the same time.\n");
  }

  if(!usediis && !useadiis && !usebroyden && !usetrrh && !linesearch && (shift==0.0)) {
    ERROR_INFO();
    throw std::runtime_error("Refusing to run calculation without an update scheme.\n");
  }

  // Nuclear repulsion
  Enuc=basis.Enuc();

  // Use density fitting?
  densityfit=set.get_bool("DensityFitting");
  // How much memory to allow (convert to bytes)
  fitmem=1000000*set.get_int("FittingMemory");
  // Linear dependence threshold
  fitthr=set.get_double("FittingThreshold");

  // Timer
  Timer t;
  Timer tinit;

  if(verbose) {
    basis.print();

    printf("\nForming overlap matrix ... ");
    fflush(stdout);
    t.set();
  }

  S=basis.overlap();

  if(verbose) {
    printf("done (%s)\n",t.elapsed().c_str());

    printf("Forming kinetic energy matrix ... ");
    fflush(stdout);
    t.set();
  }

  T=basis.kinetic();

  if(verbose) {
    printf("done (%s)\n",t.elapsed().c_str());

    printf("Forming nuclear attraction matrix ... ");
    fflush(stdout);
    t.set();
  }

  Vnuc=basis.nuclear();

  if(verbose)
    printf("done (%s)\n",t.elapsed().c_str());

  // Electric field?
  arma::mat H_E(T.n_rows,T.n_cols);
  H_E.zeros();

  std::vector<std::string> Ef=splitline(set.get_string("EField"));
  if(Ef.size()!=3)
    throw std::runtime_error("EField must have 3 components!\n");
  std::vector<double> E(Ef.size());
  for(size_t i=0;i<Ef.size();i++)
    E[i]=readdouble(Ef[i]);
  if(E[0]!=0.0 || E[1] != 0.0 || E[2] != 0.0) {
    // Compute center of charge
    coords_t cen;
    cen.x=cen.y=cen.z=0.0;
    int Ztot=0;
    for(size_t i=0;i<basis.get_Nnuc();i++) {
      nucleus_t nuc=basis.get_nucleus(i);
      if(!nuc.bsse) {
	cen=cen+nuc.r*nuc.Z;
	Ztot+=nuc.Z;
      }
    }
    cen=cen/Ztot;
    fprintf(stderr,"Center of charge at % .3f % .3f % .3f\n",cen.x/ANGSTROMINBOHR,cen.y/ANGSTROMINBOHR,cen.z/ANGSTROMINBOHR);

    // Get dipole integrals
    std::vector<arma::mat> dipint(basis.moment(1,cen.x,cen.y,cen.z));
    // Accumulate
    for(size_t i=0;i<E.size();i++)
      // H_e = - E . qr
      H_E+=E[i]*dipint[i];
  }

  // Form core Hamiltonian
  Hcore=T+Vnuc+H_E;

  if(verbose) {
    printf("\n");
    t.set();
  }

  Sinvh=BasOrth(S,set);

  if(verbose) {
    printf("Basis set diagonalized in %s.\n",t.elapsed().c_str());
    t.set();

    if(Sinvh.n_cols!=Sinvh.n_rows) {
      printf("%i linear combinations of basis functions have been removed.\n",Sinvh.n_rows-Sinvh.n_cols);
    }
    printf("\n");
  }

  if(densityfit) {
    // Form density fitting basis

    // Do we need RI-K, or is RI-J sufficient?
    bool rik=false;
    if(stricmp(set.get_string("Method"),"HF")==0)
      rik=true;
    else if(stricmp(set.get_string("Method"),"ROHF")==0)
      rik=true;
    else {
      // No hartree-fock; check if functional has exact exchange part
      int xfunc, cfunc;
      parse_xc_func(xfunc,cfunc,set.get_string("Method"));
      if(exact_exchange(xfunc)!=0.0)
	rik=true;
    }

    if(stricmp(set.get_string("FittingBasis"),"Auto")==0) {
      // Check used method
      if(rik)
	throw std::runtime_error("Automatical auxiliary basis set formation not implemented for exact exchange.\nSet the FittingBasis.\n");

      // DFT, OK for now (will be checked again later on)
      dfitbas=basisp->density_fitting();
    } else {
      // Load basis library
      BasisSetLibrary fitlib;
      fitlib.load_gaussian94(set.get_string("FittingBasis"));

      // Construct fitting basis
      construct_basis(dfitbas,basisp->get_nuclei(),fitlib,set);
    }

    // Compute memory estimate
    std::string memest=memory_size(dfit.memory_estimate(*basisp,dfitbas,direct));

    if(verbose) {
      if(direct)
	printf("Initializing density fitting calculation, requiring %s memory ... ",memest.c_str());
      else
	printf("Computing density fitting integrals, requiring %s memory ... ",memest.c_str());
      fflush(stdout);
      t.set();
    }

    // Fill the basis
    size_t Npairs=dfit.fill(*basisp,dfitbas,direct,intthr,fitthr,rik);

    if(verbose) {
      printf("done (%s)\n",t.elapsed().c_str());
      printf("%i shell pairs out of %i are significant.\n",(int) Npairs, (int) basis.get_unique_shellpairs().size());
      printf("Auxiliary basis contains %i functions.\n",(int) dfit.get_Naux());
      fflush(stdout);
    }
  } else  {
    // Compute ERIs
    if(direct) {
      size_t Npairs;
      // Form decontracted basis set and get the screening matrix
      decbas=basis.decontract(decconv);
      
      if(verbose) {
	t.set();
	printf("Forming ERI screening matrix ... ");
	fflush(stdout);
      }
      
      if(decfock)
	// Use decontracted basis
	Npairs=scr.fill(&decbas,intthr,verbose);
      else
	// Use contracted basis
	Npairs=scr.fill(&basis,intthr,verbose);
      
      if(verbose) {
	printf("done (%s)\n",t.elapsed().c_str());
	if(decfock)
	  printf("%i shell pairs out of %i are significant.\n",(int) Npairs, (int) decbas.get_unique_shellpairs().size());
	else
	  printf("%i shell pairs out of %i are significant.\n",(int) Npairs, (int) basis.get_unique_shellpairs().size());
      }

    } else {
      // Compute memory requirement
      size_t N;

      if(verbose) {
	N=tab.N_ints(&basis,intthr);
	printf("Forming table of %u ERIs, requiring %s of memory ... ",(unsigned int) N,memory_size(N*sizeof(double)).c_str());
	fflush(stdout);
      }
      // Don't compute small integrals
      size_t Npairs=tab.fill(&basis,intthr);

      if(verbose) {
	printf("done (%s)\n",t.elapsed().c_str());
	printf("%i shell pairs out of %i are significant.\n",(int) Npairs, (int) basis.get_unique_shellpairs().size());
      }
    }
  }

  if(verbose) {
    printf("\nInitialization of computation done in %s.\n\n",tinit.elapsed().c_str());
    fflush(stdout);
  }
}

SCF::~SCF() {
}

void SCF::set_frozen(const arma::mat & C, size_t ind) {
  // Check size of array
  while(ind+1>freeze.size()) {
    arma::mat tmp;
    freeze.push_back(tmp);
  }
  // Store frozen core orbitals
  freeze[ind]=C;
}

void SCF::set_fitting(const BasisSet & fitbasv) {
  dfitbas=fitbasv;
}

void SCF::do_force(bool val) {
  doforce=val;
}

bool SCF::get_verbose() const {
  return verbose;
}

void SCF::set_verbose(bool verb) {
  verbose=verb;
}

arma::mat SCF::get_S() const {
  return S;
}

arma::mat SCF::get_Sinvh() const {
  return Sinvh;
}

arma::mat SCF::get_Hcore() const {
  return Hcore;
}

Checkpoint *SCF::get_checkpoint() const {
  return chkptp;
}

void SCF::PZSIC_Fock(std::vector<arma::cx_mat> & Forb, arma::vec & Eorb, const arma::cx_mat & C, const arma::cx_mat & W, dft_t dft, DFTGrid & grid, DFTGrid & nlgrid, bool fock) {
  // Compute the orbital-dependent Fock matrices
  Eorb.resize(W.n_cols);
  if(fock)
    Forb.resize(W.n_cols);

  // Range separation constants
  double omega, kfull, kshort;
  range_separation(dft.x_func,omega,kfull,kshort);

  // Optimal orbitals
  arma::cx_mat Ctilde=C.cols(0,W.n_rows-1)*W;
  // Orbital density matrices
  std::vector<arma::cx_mat> Pcorb(Ctilde.n_cols);
  for(size_t io=0;io<Ctilde.n_cols;io++)
    Pcorb[io]=Ctilde.col(io)*arma::trans(Ctilde.col(io));

  std::vector<arma::mat> Porb(Ctilde.n_cols);
  for(size_t io=0;io<Ctilde.n_cols;io++)
    Porb[io]=arma::real(Pcorb[io]);

  Timer t;
  
  if(densityfit) {
    if(kfull!=0.0)
      throw std::runtime_error("PZ-SIC hybrid functionals not supported with density fitting.\n");
    if(kshort!=0.0)
      throw std::runtime_error("PZ-SIC range separated functionals not supported with density fitting.\n");
    
    if(verbose) {
      if(fock)
	printf("Constructing orbital Coulomb matrices ... ");
      else
	printf("Computing    orbital Coulomb energies ... ");
      fflush(stdout);
      t.set();
    }

    // Coulomb matrices
    std::vector<arma::mat> Jorb;
    Jorb=dfit.calc_J(Porb);
    for(size_t io=0;io<Ctilde.n_cols;io++)
      Eorb[io]=0.5*arma::trace(Porb[io]*Jorb[io]);
    if(fock)
      for(size_t io=0;io<Ctilde.n_cols;io++)
	Forb[io]=Jorb[io]*COMPLEX1;

    if(verbose) {
      printf("done (%s)\n",t.elapsed().c_str());
      fflush(stdout);
    }

  } else {
    if(!direct) {
      // Tabled integrals

      // Compute range separated integrals if necessary
      if(is_range_separated(dft.x_func)) {
	bool fill;
	if(!tab_rs.get_N()) {
	  fill=true;
	} else {
	  double o, kl, ks;
	  tab_rs.get_range_separation(o,kl,ks);
	  fill=(!(o==omega));
	}
	if(fill) {
	  t.set();
	  if(verbose) {
	    printf("Computing short-range repulsion integrals ... ");
	    fflush(stdout);
	  }

	  tab_rs.set_range_separation(omega,0.0,1.0);
	  size_t Np=tab_rs.fill(basisp,intthr);

	  if(verbose) {
	    printf("done (%s)\n",t.elapsed().c_str());
	    printf("%i short-range shell pairs are significant.\n",(int) Np);
	    fflush(stdout);
	  }
	}
      }

      if(verbose) {
	if(fock)
	  printf("Constructing orbital Coulomb matrices ... ");
	else
	  printf("Computing    orbital Coulomb energies ... ");
	fflush(stdout);
	t.set();
      }

      for(size_t io=0;io<Ctilde.n_cols;io++) {
	// Calculate Coulomb term
	arma::mat Jorb=tab.calcJ(Porb[io]);
	// and Coulomb energy
	Eorb[io]=0.5*arma::trace(Porb[io]*Jorb);
	if(fock)
	  Forb[io]=Jorb*COMPLEX1;
      }

      if(verbose) {
	printf("done (%s)\n",t.elapsed().c_str());
	fflush(stdout);
      }

      // Full exchange
      if(kfull!=0.0) {
	if(verbose) {
	  if(fock)
	    printf("Constructing orbital exchange matrices ... ");
	  else
	    printf("Computing    orbital exchange energies ... ");
	  fflush(stdout);
	  t.set();
	}

	for(size_t io=0;io<Ctilde.n_cols;io++) {
	  // Fock matrix
	  arma::cx_mat Korb=kfull*tab.calcK(Pcorb[io]);
	  // and energy
	  Eorb[io]-=0.5*std::real(arma::trace(Pcorb[io]*Korb));
	  if(fock)
	    Forb[io]-=Korb;
	}
	
	if(verbose) {
	  printf("done (%s)\n",t.elapsed().c_str());
	  fflush(stdout);
	}
      }
      
      // Short-range part
      if(kshort!=0.0) {
	if(verbose) {
	  if(fock)
	    printf("Constructing orbital short-range exchange matrices ... ");
	  else
	    printf("Computing    orbital short-range exchange energies ... ");
	  fflush(stdout);
	  t.set();
	}

	for(size_t io=0;io<Ctilde.n_cols;io++) {
	  // Potential and energy
	  arma::cx_mat Korb=kshort*tab_rs.calcK(Pcorb[io]);
	  Eorb[io]-=0.5*std::real(arma::trace(Pcorb[io]*Korb));
	  if(fock)
	    Forb[io]-=Korb;
	}

	if(verbose) {
	  printf("done (%s)\n",t.elapsed().c_str());
	  fflush(stdout);
	}
      }

    } else {
      // Compute range separated integrals if necessary
      if(is_range_separated(dft.x_func)) {
	bool fill;
	if(!scr_rs.get_N()) {
	  fill=true;
	} else {
	  double o, kl, ks;
	  scr_rs.get_range_separation(o,kl,ks);
	  fill=(!(o==omega));
	}
	if(fill) {
	  t.set();
	  if(verbose) {
	    printf("Computing short-range repulsion integrals ... ");
	    fflush(stdout);
	  }

	  scr_rs.set_range_separation(omega,0.0,1.0);
	  size_t Np=scr_rs.fill(basisp,intthr);

	  if(verbose) {
	    printf("done (%s)\n",t.elapsed().c_str());
	    printf("%i short-range shell pairs are significant.\n",(int) Np);
	    fflush(stdout);
	  }
	}
      }

      if(verbose) {
	std::string leg = (kfull==0.0) ? "Coulomb" : "Coulomb and exchange";
	if(fock)
	  printf("Constructing orbital %s matrices ... ",leg.c_str());
	else
	  printf("Computing    orbital %s energies ... ",leg.c_str());
	fflush(stdout);
	t.set();
      }

      // Calculate Coulomb and exchange terms
      {
	std::vector<arma::cx_mat> JKorb=scr.calcJK(Pcorb,1.0,kfull,intthr);
	for(size_t io=0;io<Ctilde.n_cols;io++) {
	  // Coulomb-exchange energy is
	  Eorb[io]=0.5*std::real(arma::trace(Pcorb[io]*JKorb[io]));
	  if(fock)
	    Forb[io]=JKorb[io];
	}
      }
      
      if(verbose) {
	printf("done (%s)\n",t.elapsed().c_str());
	fflush(stdout);
      }
      
      // Short-range part
      if(kshort!=0.0) {
	if(verbose) {
	  if(fock)
	    printf("Constructing orbital short-range exchange matrices ... ");
	  else
	    printf("Computing    orbital short-range exchange energies ... ");
	  fflush(stdout);
	  t.set();
	}
	
	// Calculate exchange term
	{
	  std::vector<arma::cx_mat> Korb=scr_rs.calcJK(Pcorb,0.0,kshort,intthr);
	  for(size_t io=0;io<Ctilde.n_cols;io++) {
	    // Short-range exchange energy is (sign already accounted for)
	    Eorb[io]+=0.5*std::real(arma::trace(Pcorb[io]*Korb[io]));
	    if(fock)
	      Forb[io]+=Korb[io];
	  }
	}
	
	if(verbose) {
	  printf("done (%s)\n",t.elapsed().c_str());
	  fflush(stdout);
	}
      }
    }
  }
  
  // Exchange-correlation
  if(dft.x_func != 0 || dft.c_func != 0) {
    if(verbose) {
      if(fock)
	printf("Constructing orbital XC matrices ... ");
      else
	printf("Computing    orbital XC energies ... ");
      fflush(stdout);
      t.set();
    }

    std::vector<double> Nelnum; // Numerically integrated density
    std::vector<arma::mat> XC; // Exchange-correlation matrices
    std::vector<double> Exc; // Exchange-correlation energy

    grid.eval_Fxc(dft.x_func,dft.c_func,C,W,XC,Exc,Nelnum,fock);

    if(verbose) {
      printf("done (%s)\n",t.elapsed().c_str());
      fflush(stdout);
    }

    if(dft.nl) {
      if(verbose) {
	if(fock)
	  printf("Constructing orbital non-local correlation matrices ... ");
	else
	  printf("Computing    orbital non-local correlation energies ... ");
	fflush(stdout);
	t.set();
      }

      // Parallellization inside VV10
      for(size_t i=0;i<Ctilde.n_cols;i++) {
	double Enl=0.0;
	arma::mat P(arma::real(Ctilde.col(i)*arma::trans(Ctilde.col(i))));
	grid.eval_VV10(nlgrid,dft.vv10_b,dft.vv10_C,P,XC[i],Enl,fock);
      }

      if(verbose) {
	printf("done (%s)\n",t.elapsed().c_str());
	fflush(stdout);
      }
    }

    // Add in the XC part to the energy
    for(size_t io=0;io<Ctilde.n_cols;io++) {
      Eorb[io]+=Exc[io];
    }

    // and the Fock matrix
    if(fock)
      for(size_t io=0;io<Ctilde.n_cols;io++)
	Forb[io]+=XC[io]*COMPLEX1;
  }
}

void SCF::PZSIC_RDFT(rscf_t & sol, const std::vector<double> & occs, dft_t dft, pzmet_t pzmet, enum pzham pzh, double pzcor, DFTGrid & grid, DFTGrid & nlgrid, double Etol, double maxtol, double rmstol, size_t niter, bool canonical, bool localization, bool real, int seed) {
  // Set xc functionals
  if(!pzmet.X)
    dft.x_func=0;
  if(!pzmet.C)
    dft.c_func=0;
  if(!pzmet.D)
    dft.nl=false;

  // Count amount of occupied orbitals
  size_t nocc=0;
  while(nocc<occs.size() && occs[nocc]!=0.0)
    nocc++;

  // Check occupations
  {
    bool ok=true;
    for(size_t i=1;i<nocc;i++)
      if(fabs(occs[i]-occs[0])>1e-6)
	ok=false;
    if(!ok)  {
      fprintf(stderr,"Occupations:");
      for(size_t i=0;i<nocc;i++)
	fprintf(stderr," %e",occs[i]);
      fprintf(stderr,"\n");

      throw std::runtime_error("SIC not supported for orbitals with varying occupations.\n");
    }
  }

  // Collect the orbitals
  rscf_t sicsol;
  sicsol.H=sol.H;
  sicsol.P=sol.P/2.0;
  sicsol.C=sol.C;
  if(sol.cC.n_rows == sol.C.n_rows && sol.cC.n_cols == sol.C.n_cols)
    sicsol.cC=sol.cC;
  else
    sicsol.cC=sol.C*COMPLEX1;

  // The localizing matrix
  arma::cx_mat W;
  if(chkptp->exist("CW.re")) {
    printf("Read localization matrix from checkpoint.\n");

    // Get old localized orbitals
    arma::cx_mat CW;
    chkptp->cread("CW",CW);
    // The starting guess is the unitarized version of the overlap
    W=unitarize(arma::trans(sicsol.cC.cols(0,CW.n_cols-1))*S*CW);
  }
  // Check that it is sane
  if(W.n_rows != nocc || W.n_cols != nocc) {
    if(canonical)
      // Use canonical orbitals
      W.eye(nocc,nocc);
    else {
      // Initialize with a random unitary matrix.
      if(real)
	W=real_orthogonal(nocc,seed)*std::complex<double>(1.0,0.0);
      else
	W=complex_unitary(nocc,seed);

      if(localization && nocc>1) {
	Timer tloc;

	// Localize starting guess
	if(verbose) printf("\nInitial localization.\n");
	double measure;
	// Max 1e5 iterations, gradient norm <= 1e-3
	orbital_localization(PIPEK_IAO2,*basisp,sicsol.C.cols(0,nocc-1),sol.P,measure,W,verbose,real,1e5,1e-3);
	if(verbose) {
	  printf("\n");

	  fprintf(stderr,"%-64s %10.3f\n","    Initial localization",tloc.get());
	  fflush(stderr);
	}

	// Initialize with Coulomb treatment?
	if(densityfit && (pzmet.X || pzmet.C || pzmet.D)) {
	  dft_t dum(dft);
	  dum.x_func=dum.c_func=0;
	  PZSIC_calculate(sicsol,W,dum,pzcor,pzh,grid,nlgrid,0.0,0.1,0.1,100,canonical,real);
	}
      }
    }
  }

  if(dft.adaptive && (pzmet.X || pzmet.C || pzmet.D)) {
    // Before proceeding, reform DFT grids so that localized orbitals
    // are properly integrated over.

    // Update Ctilde
    arma::cx_mat Ctilde=sicsol.cC.cols(0,W.n_cols-1)*W;

    // Update DFT grid
    Timer tgrid;
    if(verbose) {
      printf("\nReconstructing SIC DFT grid.\n");
      fprintf(stderr,"\n");
      fflush(stdout);
    }
    grid.construct(Ctilde,dft.gridtol,dft.x_func,dft.c_func);
    if(verbose) {
      printf("\n");
      fflush(stdout);

      fprintf(stderr,"%-65s %10.3f\n","    SIC-DFT grid formation",tgrid.get());
      fflush(stderr);
    }
  } else { // if(dft.adaptive)
    if(verbose)
      fprintf(stderr,"\n");
  }

  // Do the calculation
  Timer tsic;
  if(verbose && !canonical) {
    fprintf(stderr,"SIC unitary optimization\n");
  }
  PZSIC_calculate(sicsol,W,dft,pzcor,pzh,grid,nlgrid,Etol,maxtol,rmstol,niter,canonical,real);
  if(verbose && !canonical) {
    fprintf(stderr,"Unitary optimization performed in %s.\n\n",tsic.elapsed().c_str());

    /*
    printf("\n");
    analyze_orbitals(*basisp,sicsol.cC*W);
    printf("\n");
    */
  }
  // Save matrix
  chkptp->cwrite("CW",sicsol.cC.cols(0,W.n_rows-1)*W);
  // Save SI energies
  chkptp->write("ESIC",sicsol.E);
  // Compute projected energies
  if(sol.H.n_rows == sicsol.Heff.n_rows && sol.H.n_cols == sicsol.Heff.n_cols) {
    arma::cx_mat CW=sicsol.cC.cols(0,W.n_cols-1)*W;
    arma::vec Ep=arma::real(arma::diagvec(arma::trans(CW)*(sol.H+sicsol.Heff)*CW));
    chkptp->write("EpSIC",Ep);
  }

  // Update current solution
  sol.Heff=sicsol.Heff;
  sol.Heff_im=sicsol.Heff_im;
  if(sol.H.n_rows == sicsol.Heff.n_rows && sol.H.n_cols == sicsol.Heff.n_cols)
    sol.H  +=sicsol.Heff;
  // Remember there are two electrons in each orbital
  sol.en.Eeff=2*sicsol.en.E;
  sol.en.Eel+=2*sicsol.en.E;
  sol.en.E  +=2*sicsol.en.E;
}

void SCF::PZSIC_UDFT(uscf_t & sol, const std::vector<double> & occa, const std::vector<double> & occb, dft_t dft, pzmet_t pzmet, enum pzham pzh, double pzcor, DFTGrid & grid, DFTGrid & nlgrid, double Etol, double maxtol, double rmstol, size_t niter, bool canonical, bool localization, bool real, int seed) {
  // Set xc functionals
  // Set xc functionals
  if(!pzmet.X)
    dft.x_func=0;
  if(!pzmet.C)
    dft.c_func=0;
  if(!pzmet.D)
    dft.nl=false;

  // Count amount of occupied orbitals
  size_t nocca=0;
  while(nocca<occa.size() && occa[nocca]!=0.0)
    nocca++;
  size_t noccb=0;
  while(noccb<occb.size() && occb[noccb]!=0.0)
    noccb++;

  // Check occupations
  {
    bool ok=true;
    for(size_t i=1;i<nocca;i++)
      if(fabs(occa[i]-occa[0])>1e-6)
	ok=false;
    for(size_t i=1;i<noccb;i++)
      if(fabs(occb[i]-occb[0])>1e-6)
	ok=false;
    if(!ok) {
      fprintf(stderr,"Alpha occupations:");
      for(size_t i=0;i<nocca;i++)
	fprintf(stderr," %e",occa[i]);
      fprintf(stderr,"\n");

      fprintf(stderr,"Beta occupations:");
      for(size_t i=0;i<noccb;i++)
	fprintf(stderr," %e",occb[i]);
      fprintf(stderr,"\n");

      throw std::runtime_error("SIC not supported for orbitals with varying occupations.\n");
    }
  }

  // Collect the orbitals
  rscf_t sicsola;
  sicsola.H=sol.Ha;
  sicsola.P=sol.Pa;
  sicsola.C=sol.Ca;
  if(sol.cCa.n_rows == sol.Ca.n_rows && sol.cCa.n_cols == sol.Ca.n_cols)
    sicsola.cC=sol.cCa;
  else
    sicsola.cC=sol.Ca*COMPLEX1;

  rscf_t sicsolb;
  sicsolb.H=sol.Hb;
  sicsolb.P=sol.Pb;
  sicsolb.C=sol.Cb;
  if(sol.cCb.n_rows == sol.Cb.n_rows && sol.cCb.n_cols == sol.Cb.n_cols)
    sicsolb.cC=sol.cCb;
  else
    sicsolb.cC=sol.Cb*COMPLEX1;

  // The localizing matrix
  arma::cx_mat Wa, Wb;
  if(chkptp->exist("CWa.re")) {
    if(verbose) printf("Read alpha localization matrix from checkpoint.\n");

    // Get old localized orbitals
    arma::cx_mat CWa;
    chkptp->cread("CWa",CWa);
    // The starting guess is the unitarized version of the overlap
    Wa=unitarize(arma::trans(sicsola.cC.cols(0,CWa.n_cols-1))*S*CWa);
  }
  if(chkptp->exist("CWb.re")) {
    if(verbose) printf("Read beta localization matrix from checkpoint.\n");

    // Get old localized orbitals
    arma::cx_mat CWb;
    chkptp->cread("CWb",CWb);
    // The starting guess is the unitarized version of the overlap
    Wb=unitarize(arma::trans(sicsolb.cC.cols(0,CWb.n_cols-1))*S*CWb);
  }

  // Check that they are sane
  if(Wa.n_rows != nocca || Wa.n_cols != nocca) {
    if(canonical)
      // Use canonical orbitals
      Wa.eye(nocca,nocca);
    else {
      // Initialize with a random unitary matrix.
      if(real)
	Wa=real_orthogonal(nocca,seed)*std::complex<double>(1.0,0.0);
      else
	Wa=complex_unitary(nocca,seed);

      if(localization && nocca>1) {
	Timer tloc;

	// Localize starting guess
	if(verbose) printf("\nInitial alpha localization.\n");
	double measure;
	// Max 1e5 iterations, gradient norm <= 1e-3
	orbital_localization(PIPEK_IAO2,*basisp,sicsola.C.cols(0,nocca-1),sol.P,measure,Wa,verbose,real,1e5,1e-3);

	if(verbose) {
	  printf("\n");

	  fprintf(stderr,"%-64s %10.3f\n","    Initial alpha localization",tloc.get());
	  fflush(stderr);
	}

	// Initialize with Coulomb treatment?
	if(densityfit && (pzmet.X || pzmet.C || pzmet.D)) {
	  dft_t dum(dft);
	  dum.x_func=dum.c_func=0;
	  PZSIC_calculate(sicsola,Wa,dum,pzcor,pzh,grid,nlgrid,0.0,0.1,0.1,100,canonical,real);
	}
      }
    }
  }

  if(Wb.n_rows != noccb || Wb.n_cols != noccb) {
    if(canonical)
      // Use canonical orbitals
      Wb.eye(noccb,noccb);
    else {
      // Initialize with a random unitary matrix.
      if(real)
	Wb=real_orthogonal(noccb,seed)*std::complex<double>(1.0,0.0);
      else
	Wb=complex_unitary(noccb,seed);

      if(localization && noccb>1) {
	Timer tloc;

	// Localize starting guess with threshold 10.0
	if(verbose) printf("\nInitial beta localization.\n");
	double measure;
	// Max 1e5 iterations, gradient norm <= 1e-3
	orbital_localization(PIPEK_IAO2,*basisp,sicsolb.C.cols(0,noccb-1),sol.P,measure,Wb,verbose,real,1e5,1e-3);

	if(verbose) {
	  printf("\n");

	  fprintf(stderr,"%-64s %10.3f\n","    Initial beta localization",tloc.get());
	  fflush(stderr);
	}

	// Initialize with Coulomb treatment?
	if(densityfit && (pzmet.X || pzmet.C || pzmet.D)) {
	  dft_t dum(dft);
	  dum.x_func=dum.c_func=0;
	  PZSIC_calculate(sicsolb,Wb,dum,pzcor,pzh,grid,nlgrid,0.0,0.1,0.1,100,canonical,real);
	}
      }
    }
  }

  if(dft.adaptive && (pzmet.X || pzmet.C || pzmet.D)) {
    // Before proceeding, reform DFT grids so that localized orbitals
    // are properly integrated over.

    // Update Ctilde
    arma::cx_mat Catilde;
    if(Wa.n_rows)
      Catilde=sicsola.cC.cols(0,Wa.n_rows-1)*Wa;
    arma::cx_mat Cbtilde;
    if(Wb.n_rows)
      Cbtilde=sicsolb.cC.cols(0,Wb.n_rows-1)*Wb;

    // Update DFT grid
    Timer tgrid;
    if(verbose) {
      printf("\nReconstructing SIC DFT grid.\n");
      fflush(stdout);
      fprintf(stderr,"\n");
    }
    {
      // Combined orbitals
      arma::cx_mat Ctilde(Catilde.n_rows,Catilde.n_cols+Cbtilde.n_cols);
      if(Catilde.n_cols)
	Ctilde.cols(0,Catilde.n_cols-1)=Catilde;
      if(Cbtilde.n_cols)
	Ctilde.cols(Catilde.n_cols,Catilde.n_cols+Cbtilde.n_cols-1)=Cbtilde;
      grid.construct(Ctilde,dft.gridtol,dft.x_func,dft.c_func);
    }
    if(verbose) {
      printf("\n");
      fflush(stdout);

      fprintf(stderr,"%-65s %10.3f\n","    SIC-DFT grid formation",tgrid.get());
      fflush(stderr);
    }
  } else { // if(dft.adaptive)
    if(verbose)
      fprintf(stderr,"\n");
  }

  // Do the calculation
  Timer tsic;
  if(verbose) {
    if(!canonical && Wa.n_cols>1)
      fprintf(stderr,"SIC unitary optimization,  alpha spin\n");
    else
      fprintf(stderr,"SIC canonical calculation, alpha spin\n");
  }
  PZSIC_calculate(sicsola,Wa,dft,pzcor,pzh,grid,nlgrid,Etol,maxtol,rmstol,niter,canonical,real);
  chkptp->cwrite("CWa",sicsola.cC.cols(0,Wa.n_rows-1)*Wa);
  chkptp->write("ESICa",sicsola.E);
  // Compute projected energies
  if(sol.Ha.n_rows == sicsola.Heff.n_rows && sol.Ha.n_cols == sicsola.Heff.n_cols) {
    arma::cx_mat CW=sicsola.cC.cols(0,Wa.n_rows-1)*Wa;
    arma::vec Ep=arma::real(arma::diagvec(arma::trans(CW)*(sol.Ha+sicsola.Heff)*CW));
    chkptp->write("EpSICa",Ep);
  }

  if(Wb.n_cols) {
    if(verbose) {
      fprintf(stderr,"Unitary optimization performed in %s.\n",tsic.elapsed().c_str());
      tsic.set();

      /*
	printf("\n");
	analyze_orbitals(*basisp,sicsol.cC*W);
	printf("\n");
      */

      if(!canonical && Wb.n_cols>1)
	fprintf(stderr,"SIC unitary optimization,   beta spin\n");
      else
	fprintf(stderr,"SIC canonical calculation,  beta spin\n");
    }
    PZSIC_calculate(sicsolb,Wb,dft,pzcor,pzh,grid,nlgrid,Etol,maxtol,rmstol,niter,canonical,real);
    chkptp->cwrite("CWb",sicsolb.cC.cols(0,Wb.n_rows-1)*Wb);
    chkptp->write("ESICb",sicsolb.E);
    // Compute projected energies
  if(sol.Hb.n_rows == sicsolb.Heff.n_rows && sol.Hb.n_cols == sicsolb.Heff.n_cols) {
    arma::cx_mat CW=sicsolb.cC.cols(0,Wb.n_rows-1)*Wb;
      arma::vec Ep=arma::real(arma::diagvec(arma::trans(CW)*(sol.Hb+sicsolb.Heff)*CW));
      chkptp->write("EpSICb",Ep);
    }
  }

  if(verbose && !canonical) {
    fprintf(stderr,"Unitary optimization performed in %s.\n\n",tsic.elapsed().c_str());
    tsic.set();

    /*
      printf("\n");
      analyze_orbitals(*basisp,sicsol.cC*W);
      printf("\n");
    */
  }

  // Update current solution
  sol.Heffa=sicsola.Heff;
  sol.Heffa_im=sicsola.Heff_im;
  if(sol.Ha.n_rows == sicsola.Heff.n_rows && sol.Ha.n_cols == sicsola.Heff.n_cols)
    sol.Ha  +=sicsola.Heff;
  if(Wb.n_cols) {
    sol.Heffb=sicsolb.Heff;
    sol.Heffb_im=sicsolb.Heff_im;
    if(sol.Hb.n_rows == sicsolb.Heff.n_rows && sol.Hb.n_cols == sicsolb.Heff.n_cols)
      sol.Hb  +=sicsolb.Heff;
    sol.en.Eeff=sicsola.en.E+sicsolb.en.E;
    sol.en.Eel+=sicsola.en.E+sicsolb.en.E;
    sol.en.E  +=sicsola.en.E+sicsolb.en.E;
  } else {
    sol.en.Eeff=sicsola.en.E;
    sol.en.Eel+=sicsola.en.E;
    sol.en.E  +=sicsola.en.E;
  }
}

void SCF::PZSIC_calculate(rscf_t & sol, arma::cx_mat & W, dft_t dft, double pzcor, enum pzham pzh, DFTGrid & grid, DFTGrid & nlgrid, double Etol, double maxtol, double rmstol, size_t nmax, bool canonical, bool real) {
  // Initialize the worker
  PZSIC* worker=new PZSIC(this,dft,&grid,&nlgrid,maxtol,rmstol,pzh);
  worker->set(sol,pzcor);

  double ESIC;
  if(canonical || W.n_cols==1) {
    // Use canonical orbitals for SIC
    ESIC=worker->cost_func(W);
  } else {
    //	Perform unitary optimization, take at max nmax iterations
    if(real) {
      // Real optimization
      W=arma::real(W)*std::complex<double>(1.0,0.0);
    }
    worker->setW(W);

    // Optimizer
    UnitaryOptimizer opt(DBL_MAX,Etol,verbose,real);
    UnitaryFunction *hlp=worker;
    opt.optimize(hlp,POLY_DF,CGPR,nmax);
    worker=(PZSIC *) hlp;

    ESIC=worker->get_ESIC();
    W=worker->getW();
  }

  // Get SI energy and hamiltonian
  arma::cx_mat HSIC=worker->get_HSIC();

  // Adjust Fock operator for SIC
  sol.Heff=-pzcor*arma::real(HSIC);
  sol.Heff_im=-pzcor*arma::imag(HSIC);
  // Need to adjust energy as well as this was calculated in the Fock routines
  sol.en.E=-pzcor*ESIC;

  // Get orbital self-interaction energies
  sol.E=worker->get_Eorb();

  // Projected energies
  arma::vec Ep;
  // Sort index
  arma::uvec idx;

  if(sol.H.n_rows == sol.Heff.n_rows && sol.H.n_cols == sol.Heff.n_cols) {
    // Sort orbitals wrt projected energy
    arma::cx_mat CW=sol.cC.cols(0,W.n_cols-1)*W;
    Ep=arma::real(arma::diagvec(arma::trans(CW)*(sol.H+sol.Heff)*CW));
    // Get index
    idx=arma::stable_sort_index(Ep);
    // Rearrange everything
    Ep=Ep(idx);
  } else {
    // Sort orbitals wrt decreasing SI energy
    idx=arma::stable_sort_index(sol.E,"descend");
  }

  sol.E=sol.E(idx);
  W=W.cols(idx);

  // Get orbital self-interaction energies
  if(verbose) {
    printf("Self-interaction energy is %e.\n",ESIC);

    printf("Decomposition of self-interaction energies:\n");
    printf("\t%4s\t%8s\t%8s\n","io","E(orb)","E(SI)");
    if(Ep.n_elem == sol.E.n_elem) {
      for(size_t io=0;io<sol.E.n_elem;io++)
	printf("\t%4i\t% 8.3f\t% 8.3f\n",(int) io+1,Ep(io),sol.E(io));
    } else {
      for(size_t io=0;io<sol.E.n_elem;io++)
	printf("\t%4i\t%8s\t% 8.3f\n",(int) io+1,"",sol.E(io));
    }
    fflush(stdout);
  }

  delete worker;
}

void SCF::core_guess(rscf_t & sol) const {
  // Get core Hamiltonian
  sol.H=Hcore;
  // and diagonalize it to get the orbitals
  diagonalize(S,Sinvh,sol);
}

void SCF::core_guess(uscf_t & sol) const {
  // Get core Hamiltonian
  sol.Ha=Hcore;
  sol.Hb=Hcore;
  // and diagonalize it to get the orbitals
  diagonalize(S,Sinvh,sol);
}

void SCF::gwh_guess(rscf_t & sol) const {
  // Initialize matrix
  sol.H=Hcore;
  for(size_t i=0;i<Hcore.n_rows;i++) {
    sol.H(i,i)=Hcore(i,i);
    for(size_t j=0;j<Hcore.n_cols;j++) {
      sol.H(i,j)=0.875*S(i,j)*(Hcore(i,i)+Hcore(j,j));
      sol.H(j,i)=sol.H(i,j);
    }
  }
  diagonalize(S,Sinvh,sol);
}

void SCF::gwh_guess(uscf_t & sol) const {
  // Initialize matrix
  sol.Ha=Hcore;
  for(size_t i=0;i<Hcore.n_rows;i++) {
    sol.Ha(i,i)=Hcore(i,i);
    for(size_t j=0;j<i;j++) {
      sol.Ha(i,j)=0.875*S(i,j)*(Hcore(i,i)+Hcore(j,j));
      sol.Ha(j,i)=sol.Ha(i,j);
    }
  }
  sol.Hb=sol.Ha;
  diagonalize(S,Sinvh,sol);
}

bool SCF::get_real_cmos() const {
  return realcmos;
}

void SCF::set_real_cmos(bool real) {
  realcmos=real;
}

void imag_lost(const rscf_t & sol, const arma::mat & S, double & d) {
  // Compute amount of electrons
  int Nel=(int) round(arma::trace(S*sol.P))/2;

  // MO overlap matrix
  if(sol.cC.n_cols == sol.C.n_cols) {
    arma::cx_mat MOovl=arma::trans(sol.C.cols(0,Nel-1))*S*sol.cC.cols(0,Nel-1);

    // Amount of electrons lost in the approximation
    d=2.0*(Nel-std::real(arma::trace(MOovl*arma::trans(MOovl))));
  } else
    d=0.0;
}

void imag_lost(const uscf_t & sol, const arma::mat & S, double & da, double & db) {
  // Compute amount of electrons
  int Nela=(int) round(arma::trace(S*sol.Pa));
  int Nelb=(int) round(arma::trace(S*sol.Pb));

  // MO overlap matrix
  if(sol.cCa.n_cols == sol.Ca.n_cols) {
    arma::cx_mat MOovla=arma::trans(sol.Ca.cols(0,Nela-1))*S*sol.cCa.cols(0,Nela-1);
    da=Nela-std::real(arma::trace(MOovla*arma::trans(MOovla)));
  } else
    da=0.0;

  if(sol.cCb.n_cols == sol.Cb.n_cols) {
    arma::cx_mat MOovlb=arma::trans(sol.Cb.cols(0,Nelb-1))*S*sol.cCb.cols(0,Nelb-1);
    db=Nelb-std::real(arma::trace(MOovlb*arma::trans(MOovlb)));
  } else
    db=0.0;
}

template<typename T> void diagonalize_wrk(const arma::mat & S, const arma::mat & Sinvh, const arma::mat & P, const arma::Mat<T> & H, double shift, arma::Mat<T> & C, arma::vec & E) {
  // Transform Hamiltonian into orthogonal basis
  arma::Mat<T> Horth;
  if(shift==0.0)
    Horth=arma::trans(Sinvh)*H*Sinvh;
  else
    Horth=arma::trans(Sinvh)*(H-shift*S*P/2.0*S)*Sinvh;
  // Update orbitals and energies
  arma::Mat<T> orbs;
  eig_sym_ordered_wrk(E,orbs,Horth);
  // Transform back to non-orthogonal basis
  C=Sinvh*orbs;

  if(shift!=0.0) {
    // Orbital energies occupied by shift, so recompute these
    E=arma::real(arma::diagvec(arma::trans(C)*H*C));
  }
}

void diagonalize(const arma::mat & S, const arma::mat & Sinvh, rscf_t & sol, double shift) {
  diagonalize_wrk<double>(S,Sinvh,sol.P/2.0,sol.H,shift,sol.C,sol.E);
  check_orth(sol.C,S,false);

  // Complex orbitals?
  if(sol.Heff_im.n_rows == sol.H.n_rows && sol.Heff_im.n_cols == sol.H.n_cols) {
    // Generate complex hamiltonian
    arma::cx_mat Hc(sol.H*COMPLEX1 + sol.Heff_im*COMPLEXI);
    if(sol.K_im.n_rows == sol.H.n_rows && sol.K_im.n_cols == sol.H.n_cols)
      Hc+=sol.K_im*COMPLEXI;

    arma::vec Etmp;
    diagonalize_wrk< std::complex<double> >(S,Sinvh,sol.P/2.0,Hc,shift,sol.cC,Etmp);
  } else {
    arma::cx_mat Ch;
    sol.cC=Ch;
  }
}

void diagonalize(const arma::mat & S, const arma::mat & Sinvh, uscf_t & sol, double shift) {
  diagonalize_wrk<double>(S,Sinvh,sol.Pa,sol.Ha,shift,sol.Ca,sol.Ea);
  check_orth(sol.Ca,S,false);

  // Complex orbitals?
  if(sol.Heffa_im.n_rows == sol.Ha.n_rows && sol.Heffa_im.n_cols == sol.Ha.n_cols) {
    // Generate complex hamiltonian
    arma::cx_mat Hc(sol.Ha*COMPLEX1 + sol.Heffa_im*COMPLEXI);
    if(sol.Ka_im.n_rows == sol.Ha.n_rows && sol.Ka_im.n_cols == sol.Ha.n_cols)
      Hc+=sol.Ka_im*COMPLEXI;

    arma::vec Etmp;
    diagonalize_wrk< std::complex<double> >(S,Sinvh,sol.Pa,Hc,shift,sol.cCa,Etmp);
  } else {
    arma::cx_mat Ch;
    sol.cCa=Ch;
  }

  diagonalize_wrk<double>(S,Sinvh,sol.Pb,sol.Hb,shift,sol.Cb,sol.Eb);
  check_orth(sol.Cb,S,false);

  // Complex orbitals?
  if(sol.Heffb_im.n_rows == sol.Hb.n_rows && sol.Heffb_im.n_cols == sol.Hb.n_cols) {
    // Generate complex hamiltonian
    arma::cx_mat Hc(sol.Hb*COMPLEX1 + sol.Heffb_im*COMPLEXI);
    if(sol.Kb_im.n_rows == sol.Hb.n_rows && sol.Kb_im.n_cols == sol.Hb.n_cols)
      Hc+=sol.Kb_im*COMPLEXI;

    arma::vec Etmp;
    diagonalize_wrk< std::complex<double> >(S,Sinvh,sol.Pb,Hc,shift,sol.cCb,Etmp);
  } else {
    arma::cx_mat Ch;
    sol.cCb=Ch;
  }
}


void ROHF_update(arma::mat & Fa_AO, arma::mat & Fb_AO, const arma::mat & P_AO, const arma::mat & S, std::vector<double> occa, std::vector<double> occb, bool verbose) {
  /*
   * T. Tsuchimochi and G. E. Scuseria, "Constrained active space
   * unrestricted mean-field methods for controlling
   * spin-contamination", J. Chem. Phys. 134, 064101 (2011).
   */

  Timer t;

  arma::vec occs;
  arma::mat AO_to_NO;
  arma::mat NO_to_AO;
  form_NOs(P_AO,S,AO_to_NO,NO_to_AO,occs);

  // Construct \Delta matrix in AO basis
  arma::mat Delta_AO=(Fa_AO-Fb_AO)/2.0;

  // and take it to the NO basis.
  arma::mat Delta_NO=arma::trans(AO_to_NO)*Delta_AO*AO_to_NO;

  // Get rid of trailing zeros
  while(occa[occa.size()-1]==0.0)
    occa.erase(occa.begin()+occa.size()-1);
  while(occb[occb.size()-1]==0.0)
    occb.erase(occb.begin()+occb.size()-1);

  // Amount of independent orbitals is
  size_t Nind=AO_to_NO.n_cols;
  // Amount of core orbitals is
  size_t Nc=std::min(occa.size(),occb.size());
  // Amount of active space orbitals is
  size_t Na=std::max(occa.size(),occb.size())-Nc;
  // Amount of virtual orbitals (in NO space) is
  size_t Nv=Nind-Na-Nc;

  // Form lambda by flipping the signs of the cv and vc blocks and
  // zeroing out everything else.
  arma::mat lambda_NO(Delta_NO);
  /*
    eig_sym_ordered puts the NOs in the order of increasing
    occupation. Thus, the lowest Nv orbitals belong to the virtual
    space, the following Na to the active space and the last Nc to the
    core orbitals.
  */
  // Zero everything
  lambda_NO.zeros();
  // and flip signs of cv and vc blocks from Delta
  for(size_t c=0;c<Nc;c++) // Loop over core orbitals
    for(size_t v=Nind-Nv;v<Nind;v++) { // Loop over virtuals
      lambda_NO(c,v)=-Delta_NO(c,v);
      lambda_NO(v,c)=-Delta_NO(v,c);
    }

  // Lambda in AO is
  arma::mat lambda_AO=arma::trans(NO_to_AO)*lambda_NO*NO_to_AO;

  // Update Fa and Fb
  Fa_AO+=lambda_AO;
  Fb_AO-=lambda_AO;

  if(verbose)
    printf("Performed CUHF update of Fock operators in %s.\n",t.elapsed().c_str());
}

void determine_occ(arma::vec & nocc, const arma::mat & C, const arma::vec & nocc_old, const arma::mat & C_old, const arma::mat & S) {
  nocc.zeros();

  // Loop over states
  for(size_t i=0;i<nocc_old.n_elem;i++)
    if(nocc_old[i]!=0.0) {

      arma::vec hlp=S*C_old.col(i);

      // Determine which state is the closest to the old one
      size_t loc=0;
      double Smax=0.0;

      for(size_t j=0;j<C.n_cols;j++) {
	double ovl=arma::dot(C.col(j),hlp);
	if(fabs(ovl)>Smax) {
	  Smax=fabs(ovl);
	  loc=j;
	}
      }

      // Copy occupancy
      if(nocc[loc]!=0.0)
	printf("Problem in determine_occ: state %i was already occupied by %g electrons!\n",(int) loc,nocc[loc]);
      nocc[loc]+=nocc_old[i];
    }
}

arma::mat form_density(const arma::mat & C, int nocc) {
  arma::vec occs(C.n_cols);
  if(nocc)
    occs.subvec(0,nocc-1).ones();
  return form_density(C,occs);
}

arma::mat form_density(const arma::mat & C, const arma::vec & occs0) {
  arma::vec occs(C.n_cols);
  occs.zeros();
  if(occs0.n_elem) {
    size_t nel=std::min(occs.n_elem,occs0.n_elem);
    occs.subvec(0,nel-1)=occs0.subvec(0,nel-1);
  }
  return C*arma::diagmat(occs)*arma::trans(C);
}

void form_density(rscf_t & sol, size_t nocc) {
  arma::vec occs(sol.C.n_cols);
  occs.zeros();
  if(nocc)
    occs.subvec(0,nocc-1)=2.0*arma::ones(nocc);
  form_density(sol,occs);
}

void form_density(rscf_t & sol, const arma::vec & occs0) {
  arma::vec occs(sol.C.n_cols);
  occs.zeros();
  if(occs0.n_elem) {
    size_t nel=std::min(occs.n_elem,occs0.n_elem);
    occs.subvec(0,nel-1)=occs0.subvec(0,nel-1);
  }
  
  if(sol.cC.n_cols == sol.C.n_cols) {
    // Use complex orbitals
    arma::cx_mat cP=sol.cC*arma::diagmat(occs)*arma::trans(sol.cC);
    sol.P=arma::real(cP);
    sol.P_im=arma::imag(cP);
  } else {
    // Use real orbitals
    sol.P=sol.C*arma::diagmat(occs)*arma::trans(sol.C);
    sol.P_im.clear();
  }
  sol.rP=sol.C*arma::diagmat(occs)*arma::trans(sol.C);
}

void form_density(uscf_t & sol, const arma::vec & occa0, const arma::vec & occb0) {
  arma::vec occa(sol.Ca.n_cols);
  occa.zeros();
  if(occa0.n_elem) {
    size_t nel=std::min(occa.n_elem,occa0.n_elem);
    occa.subvec(0,nel-1)=occa0.subvec(0,nel-1);
  }
  arma::vec occb(sol.Cb.n_cols);
  occb.zeros();
  if(occb0.n_elem) {
    size_t nel=std::min(occb.n_elem,occb0.n_elem);
    occb.subvec(0,nel-1)=occb0.subvec(0,nel-1);
  }

  if(sol.cCa.n_cols == sol.Ca.n_cols) {
    // Use complex orbitals
    arma::cx_mat cPa=sol.cCa*arma::diagmat(occa)*arma::trans(sol.cCa);
    sol.Pa=arma::real(cPa);
    sol.Pa_im=arma::imag(cPa);
  } else {
    // Use real orbitals
    sol.Pa=sol.Ca*arma::diagmat(occa)*arma::trans(sol.Ca);
    sol.Pa_im.clear();
  }
  sol.rPa=sol.Ca*arma::diagmat(occa)*arma::trans(sol.Ca);

  if(sol.cCb.n_cols == sol.Cb.n_cols) {
    // Use complex orbitals
    arma::cx_mat cPb=sol.cCb*arma::diagmat(occb)*arma::trans(sol.cCb);
    sol.Pb=arma::real(cPb);
    sol.Pb_im=arma::imag(cPb);
  } else {
    // Use real orbitals
    sol.Pb=sol.Cb*arma::diagmat(occb)*arma::trans(sol.Cb);
    sol.Pb_im.clear();
  }
  sol.rPb=sol.Cb*arma::diagmat(occb)*arma::trans(sol.Cb);

  sol.P=sol.Pa+sol.Pb;
}

arma::mat form_density(const arma::vec & E, const arma::mat & C, const std::vector<double> & nocc) {
  if(nocc.size()>C.n_cols) {
    std::ostringstream oss;
    oss << "Error in function " << __FUNCTION__ << " (file " << __FILE__ << ", near line " << __LINE__ << "): there should be " << nocc.size() << " occupied orbitals but only " << C.n_cols << " orbitals exist!\n";
    throw std::runtime_error(oss.str());
  }

  if(E.n_elem != C.n_cols) {
    std::ostringstream oss;
    oss << "Error in function " << __FUNCTION__ << " (file " << __FILE__ << ", near line " << __LINE__ << "): " << E.size() << " energies but " << C.n_cols << " orbitals!\n";
    throw std::runtime_error(oss.str());
  }

  // Zero matrix
  arma::mat W(C.n_rows,C.n_rows);
  W.zeros();
  // Formulate density
  for(size_t n=0;n<nocc.size();n++)
    if(nocc[n]>0.0)
      W+=nocc[n]*E(n)*C.col(n)*arma::trans(C.col(n));

  return W;
}

arma::mat purify_density(const arma::mat & P, const arma::mat & S) {
  // McWeeny purification
  arma::mat PS=P*S;
  return 3.0*PS*P - 2.0*PS*PS*P;
}

arma::mat purify_density_NO(const arma::mat & P, const arma::mat & S) {
  arma::mat C;
  return purify_density_NO(P,C,S);
}

arma::mat purify_density_NO(const arma::mat & P, arma::mat & C, const arma::mat & S) {
  // Number of electrons
  int Nel=(int) round(arma::trace(P*S));

  // Get the natural orbitals
  arma::vec occs;
  form_NOs(P,S,C,occs);

  // and form the density
  return form_density(C,Nel);
}

std::vector<double> atomic_occupancy(int Nel) {
  std::vector<double> ret;

  // Fill shells. Shell index
  for(size_t is=0;is<sizeof(shell_order)/sizeof(shell_order[0]);is++) {
    // am of current shell is
    int l=shell_order[is];
    // and it has 2l+1 orbitals
    int nsh=2*l+1;
    // Amount of electrons to put is
    int nput=std::min(nsh,Nel);

    // and they are distributed equally
    for(int i=0;i<nsh;i++)
      ret.push_back(nput*1.0/nsh);

    // Reduce electron count
    Nel-=nput;
    if(Nel==0)
      break;
  }

  return ret;
}

std::vector<double> get_restricted_occupancy(const Settings & set, const BasisSet & basis, bool atomic) {
  // Returned value
  std::vector<double> ret;

  // Occupancies
  std::string occs=set.get_string("Occupancies");

  // Parse occupancies
  if(occs.size()) {
    // Split input
    std::vector<std::string> occvals=splitline(occs);
    // Resize output
    ret.resize(occvals.size());
    // Parse occupancies
    for(size_t i=0;i<occvals.size();i++)
      ret[i]=readdouble(occvals[i]);

    /*
    printf("Occupancies\n");
    for(size_t i=0;i<ret.size();i++)
      printf("%.2f ",ret[i]);
    printf("\n");
    */
  } else {
    // Aufbau principle.
    int Nel=basis.Ztot()-set.get_int("Charge");
    if(Nel%2!=0) {
      throw std::runtime_error("Refusing to run restricted calculation on unrestricted system!\n");
    }

    if(atomic && basis.get_Nnuc()==1) {
      // Atomic case.
      ret=atomic_occupancy(Nel/2);
      // Orbitals are doubly occupied
      for(size_t i=0;i<ret.size();i++)
	ret[i]*=2.0;
    } else {
      // Resize output
      ret.resize(Nel/2);
      for(size_t i=0;i<ret.size();i++)
	ret[i]=2.0; // All orbitals doubly occupied
    }
  }

  return ret;
}

void get_unrestricted_occupancy(const Settings & set, const BasisSet & basis, std::vector<double> & occa, std::vector<double> & occb, bool atomic) {
  // Occupancies
  std::string occs=set.get_string("Occupancies");

  // Parse occupancies
  if(occs.size()) {
    // Split input
    std::vector<std::string> occvals=splitline(occs);
    if(occvals.size()%2!=0) {
      throw std::runtime_error("Error - specify both alpha and beta occupancies for all states!\n");
    }

    // Resize output vectors
    occa.resize(occvals.size()/2);
    occb.resize(occvals.size()/2);
    // Parse occupancies
    for(size_t i=0;i<occvals.size()/2;i++) {
      occa[i]=readdouble(occvals[2*i]);
      occb[i]=readdouble(occvals[2*i+1]);
    }

    /*
    printf("Occupancies\n");
    printf("alpha\t");
    for(size_t i=0;i<occa.size();i++)
      printf("%.2f ",occa[i]);
    printf("\nbeta\t");
    for(size_t i=0;i<occb.size();i++)
      printf("%.2f ",occb[i]);
    printf("\n");
    */
  } else {
    // Aufbau principle. Get amount of alpha and beta electrons.

    int Nel_alpha, Nel_beta;
    get_Nel_alpha_beta(basis.Ztot()-set.get_int("Charge"),set.get_int("Multiplicity"),Nel_alpha,Nel_beta);

    if(atomic && basis.get_Nnuc()==1) {
      // Atomic case
      occa=atomic_occupancy(Nel_alpha);
      occb=atomic_occupancy(Nel_beta);
    } else {
      // Resize output
      occa.resize(Nel_alpha);
      for(size_t i=0;i<occa.size();i++)
	occa[i]=1.0;

      occb.resize(Nel_beta);
      for(size_t i=0;i<occb.size();i++)
	occb[i]=1.0;
    }
  }
}

double dip_mom(const arma::mat & P, const BasisSet & basis) {
  // Compute magnitude of dipole moment

  arma::vec dp=dipole_moment(P,basis);
  return arma::norm(dp,2);
}

arma::vec dipole_moment(const arma::mat & P, const BasisSet & basis) {
  // Get moment matrix
  std::vector<arma::mat> mommat=basis.moment(1);

  // Electronic part
  arma::vec el(3);
  // Compute dipole moments
  for(int i=0;i<3;i++) {
    // Electrons have negative charge
    el[i]=arma::trace(-P*mommat[i]);
  }

  //  printf("Electronic dipole moment is %e %e %e.\n",el(0),el(1),el(2));

  // Compute center of nuclear charge
  arma::vec nc(3);
  nc.zeros();
  for(size_t i=0;i<basis.get_Nnuc();i++) {
    // Get nucleus
    nucleus_t nuc=basis.get_nucleus(i);
    // Increment
    nc(0)+=nuc.Z*nuc.r.x;
    nc(1)+=nuc.Z*nuc.r.y;
    nc(2)+=nuc.Z*nuc.r.z;
  }
  //  printf("Nuclear dipole moment is %e %e %e.\n",nc(0),nc(1),nc(2));

  arma::vec ret=el+nc;

  return ret;
}

double electron_spread(const arma::mat & P, const BasisSet & basis) {
  // Compute <r^2> of density

  // Get number of electrons.
  std::vector<arma::mat> mom0=basis.moment(0);
  double Nel=arma::trace(P*mom0[0]);

  // Normalize P
  arma::mat Pnorm=P/Nel;

  // First, get <r>.
  std::vector<arma::mat> mom1=basis.moment(1);
  arma::vec r(3);
  r(0)=arma::trace(Pnorm*mom1[getind(1,0,0)]);
  r(1)=arma::trace(Pnorm*mom1[getind(0,1,0)]);
  r(2)=arma::trace(Pnorm*mom1[getind(0,0,1)]);

  //  printf("Center of electron cloud is at %e %e %e.\n",r(0),r(1),r(2));

  // Then, get <r^2> around r
  std::vector<arma::mat> mom2=basis.moment(2,r(0),r(1),r(2));
  double r2=arma::trace(Pnorm*(mom2[getind(2,0,0)]+mom2[getind(0,2,0)]+mom2[getind(0,0,2)]));

  double dr=sqrt(r2);

  return dr;
}

void get_Nel_alpha_beta(int Nel, int mult, int & Nel_alpha, int & Nel_beta) {
  // Check sanity of arguments
  if(mult<1)
    throw std::runtime_error("Invalid value for multiplicity, which must be >=1.\n");
  else if(Nel%2==0 && mult%2!=1) {
    std::ostringstream oss;
    oss << "Requested multiplicity " << mult << " with " << Nel << " electrons.\n";
    throw std::runtime_error(oss.str());
  } else if(Nel%2==1 && mult%2!=0) {
    std::ostringstream oss;
    oss << "Requested multiplicity " << mult << " with " << Nel << " electrons.\n";
    throw std::runtime_error(oss.str());
  }

  if(Nel%2==0)
    // Even number of electrons, the amount of spin up is
    Nel_alpha=Nel/2+(mult-1)/2;
  else
    // Odd number of electrons, the amount of spin up is
    Nel_alpha=Nel/2+mult/2;

  // The rest are spin down
  Nel_beta=Nel-Nel_alpha;

  if(Nel_alpha<0) {
    std::ostringstream oss;
    oss << "A multiplicity of " << mult << " would mean " << Nel_alpha << " alpha electrons!\n";
    throw std::runtime_error(oss.str());
  } else if(Nel_beta<0) {
    std::ostringstream oss;
    oss << "A multiplicity of " << mult << " would mean " << Nel_beta << " beta electrons!\n";
    throw std::runtime_error(oss.str());
  }
}

enum pzrun parse_pzrun(const std::string & pzs) {
  enum pzrun pz;

  // Perdew-Zunger SIC?
  if(stricmp(pzs,"Full")==0)
    pz=FULL;
  else if(stricmp(pzs,"OldFull")==0)
    pz=OLDFULL;
  else if(stricmp(pzs,"Pert")==0)
    pz=PERT;
  else if(stricmp(pzs,"Real")==0)
    pz=REAL;
  else if(stricmp(pzs,"RealPert")==0)
    pz=REALPERT;
  else if(stricmp(pzs,"Can")==0)
    pz=CAN;
  else if(stricmp(pzs,"CanPert")==0)
    pz=CANPERT;
  else
    pz=NO;

  return pz;
}

pzmet_t parse_pzmet(const std::string & pzmod) {
  pzmet_t mode;
  mode.X=false;
  mode.C=false;
  mode.D=false;

  for(size_t i=0;i<pzmod.size();i++)
    switch(pzmod[i]) {
    case('x'):
    case('X'):
      mode.X=true;
    break;

    case('c'):
    case('C'):
      mode.C=true;
    break;

    case('d'):
    case('D'):
      mode.D=true;
    break;

    case(' '):
    case('\0'):
      break;

    default:
      throw std::runtime_error("Invalid PZmode\n");
    }

  return mode;
}

enum pzham parse_pzham(const std::string & pzh) {
  enum pzham ham;

  if(stricmp(pzh,"Symm")==0)
    ham=PZSYMM;
  else if(stricmp(pzh,"United")==0)
    ham=PZUNITED;
  else {
    std::ostringstream oss;
    oss << "Unknown PZ-SIC Hamiltonian \"" << pzh << "\"!\n";
    throw std::runtime_error(oss.str());
  }

  return ham;
}

dft_t parse_dft(const Settings & set, bool init) {
  dft_t dft;
  dft.gridtol=0.0;
  dft.nl=false;
  dft.vv10_b=0.0;
  dft.vv10_C=0.0;

  // Use Lobatto quadrature?
  dft.lobatto=set.get_bool("DFTLobatto");

  // Tolerance
  std::string tolkw = init ? "DFTInitialTol" : "DFTFinalTol";

  // Use static grid?
  if(stricmp(set.get_string("DFTGrid"),"Auto")!=0) {
    std::vector<std::string> opts=splitline(set.get_string("DFTGrid"));
    if(opts.size()!=2) {
      throw std::runtime_error("Invalid DFT grid specified.\n");
    }

    dft.adaptive=false;
    dft.nrad=readint(opts[0]);
    dft.lmax=readint(opts[1]);
    if(dft.nrad<1 || dft.lmax==0) {
      throw std::runtime_error("Invalid DFT radial grid specified.\n");
    }
    
    // Check if l was given in number of points
    if(dft.lmax<0) {
      // Try to find corresponding Lebedev grid
      for(size_t i=0;i<sizeof(lebedev_degrees)/sizeof(lebedev_degrees[0]);i++)
	if(lebedev_degrees[i]==-dft.lmax) {
	  dft.lmax=lebedev_orders[i];
	  break;
	}
      if(dft.lmax<0)
	throw std::runtime_error("Invalid DFT angular grid specified.\n");
    }
    
  } else {
    dft.adaptive=true;
    dft.gridtol=set.get_double(tolkw);
  }

  // Parse functionals
  parse_xc_func(dft.x_func,dft.c_func,set.get_string("Method"));

  // Parse VV10
  std::string vv10s(set.get_string("VV10"));
  if(stricmp(vv10s,"Auto")==0) {
    // Determine automatically if VV10 is necessary
    if(dft.x_func>0)
      dft.nl=needs_VV10(dft.x_func,dft.vv10_b,dft.vv10_C);
    if(!dft.nl && dft.c_func>0)
      dft.nl=needs_VV10(dft.c_func,dft.vv10_b,dft.vv10_C);
    
  } else if(stricmp(vv10s,"True")==0 || stricmp(vv10s,"Yes")==0) {
    dft.nl=true;
    
    std::vector<std::string> vvopts=splitline(set.get_string("VV10Pars"));
    if(vvopts.size()!=2)
      throw std::runtime_error("Invalid VV10Pars!\n");
    
    dft.vv10_b=readdouble(vvopts[0]);
    dft.vv10_C=readdouble(vvopts[1]);

  } else if(stricmp(vv10s,"False")==0 || stricmp(vv10s,"No")==0) {
    // Do nothing
    
  } else if(vv10s.size()) {
    
    throw std::runtime_error("Error parsing VV10 setting.\n");
  }
  
  if(dft.nl) {
    if(dft.vv10_b <= 0.0 || dft.vv10_C <= 0.0) {
      std::ostringstream oss;
      oss << "VV10 parameters given b = " << dft.vv10_b << ", C = " << dft.vv10_C << " are not valid.\n";
      throw std::runtime_error(oss.str());
    }
    
    if(dft.adaptive)
      throw std::runtime_error("Adaptive DFT grids not supported with VV10.\n");
    
    std::vector<std::string> opts=splitline(set.get_string("NLGrid"));
    dft.nlnrad=readint(opts[0]);
    dft.nllmax=readint(opts[1]);
    if(dft.nlnrad<1 || dft.nllmax==0) {
      throw std::runtime_error("Invalid DFT radial grid specified.\n");
    }
    
    // Check if l was given in number of points
    if(dft.nllmax<0) {
      // Try to find corresponding Lebedev grid
      for(size_t i=0;i<sizeof(lebedev_degrees)/sizeof(lebedev_degrees[0]);i++)
	if(lebedev_degrees[i]==-dft.nllmax) {
	  dft.nllmax=lebedev_orders[i];
	  break;
	}
      if(dft.nllmax<0)
	throw std::runtime_error("Invalid DFT angular grid specified.\n");
    }
    
    // Check that xc grid is larger than nl grid
    if(dft.nrad < dft.nlnrad || dft.lmax < dft.nllmax)
      throw std::runtime_error("xc grid should be bigger than nl grid!\n");
  }    

  return dft;
}

void calculate(const BasisSet & basis, const Settings & set, bool force) {
  // Checkpoint files to load and save
  std::string loadname=set.get_string("LoadChk");
  std::string savename=set.get_string("SaveChk");

  bool verbose=set.get_bool("Verbose");

  // Print out settings
  if(verbose)
    set.print();

  // Number of electrons is
  int Nel=basis.Ztot()-set.get_int("Charge");

  // Do a plain Hartree-Fock calculation?
  bool hf= (stricmp(set.get_string("Method"),"HF")==0);
  bool rohf=(stricmp(set.get_string("Method"),"ROHF")==0);

  // Final convergence settings
  convergence_t conv;
  conv.deltaEmax=set.get_double("DeltaEmax");
  conv.deltaPmax=set.get_double("DeltaPmax");
  conv.deltaPrms=set.get_double("DeltaPrms");

  // Get exchange and correlation functionals
  dft_t dft;
  dft_t initdft;
  // Initial convergence settings
  convergence_t initconv(conv);

  if(!hf && !rohf) {
    parse_xc_func(dft.x_func,dft.c_func,set.get_string("Method"));

    initdft=parse_dft(set,true);
    dft=parse_dft(set,false);

    initconv.deltaEmax*=set.get_double("DFTDelta");
    initconv.deltaPmax*=set.get_double("DFTDelta");
    initconv.deltaPrms*=set.get_double("DFTDelta");
  }

  // Check consistency of parameters
  if(!hf && !rohf && (exact_exchange(dft.x_func)!=0.0 || is_range_separated(dft.x_func)))
    if(set.get_bool("DensityFitting") && (stricmp(set.get_string("FittingBasis"),"Auto")==0)) {
      throw std::runtime_error("Automatical auxiliary basis set formation not implemented for exact exchange.\nChange the FittingBasis.\n");
    }

  // Load starting guess?
  bool doload=(stricmp(loadname,"")!=0);
  BasisSet oldbas;
  bool oldrestr;
  arma::vec Eold, Eaold, Ebold;
  arma::mat Cold, Caold, Cbold;
  arma::mat Pold;

  arma::cx_mat CW, CWa, CWb;
  bool doCW=false;

  // Strict integrals?
  bool strictint(set.get_bool("StrictIntegrals"));
  
  // Which guess to use
  enum guess_t guess=parse_guess(set.get_string("Guess"));
  // Freeze core orbitals?
  bool freezecore=set.get_bool("FreezeCore");
  if(freezecore && guess==COREGUESS)
    throw std::runtime_error("Cannot freeze core orbitals with core guess!\n");
  if(freezecore && guess==GWHGUESS)
    throw std::runtime_error("Cannot freeze core orbitals with GWH guess!\n");

  if(doload) {
    Checkpoint load(loadname,false);

    // Basis set
    load.read(oldbas);

    // Restricted calculation?
    load.read("Restricted",oldrestr);

    // Density matrix
    load.read("P",Pold);

    if(oldrestr) {
      // Load energies and orbitals
      load.read("C",Cold);
      load.read("E",Eold);

      if(load.exist("CW.re")) {
	arma::cx_mat CWold;
	load.cread("CW",CWold);

	basis.projectOMOs(oldbas,CWold,CW);
	doCW=true;
      }
    } else {
      // Load energies and orbitals
      load.read("Ca",Caold);
      load.read("Ea",Eaold);
      load.read("Cb",Cbold);
      load.read("Eb",Ebold);

      if(load.exist("CWa.re") && load.exist("CWb.re")) {
	arma::cx_mat CWaold, CWbold;
	load.cread("CWa",CWaold);
	load.cread("CWb",CWbold);

	basis.projectOMOs(oldbas,CWaold,CWa);
	basis.projectOMOs(oldbas,CWbold,CWb);
	doCW=true;
      }
    }
  }

  if(set.get_int("Multiplicity")==1 && Nel%2==0 && !set.get_bool("ForcePol")) {
    // Closed shell case
    rscf_t sol;
    // Initialize energy
    memset(&sol.en, 0, sizeof(energy_t));

    // Project old solution to new basis
    if(doload) {
      // Restricted calculation wanted but loaded spin-polarized one
      if(!oldrestr) {
	// Find out natural orbitals
	arma::vec occs;
	form_NOs(Pold,oldbas.overlap(),Cold,occs);

	// Use alpha orbital energies
	Eold=Eaold;
      }

      // Orbitals
      basis.projectMOs(oldbas,Eold,Cold,sol.E,sol.C);
    } else if(guess == ATOMGUESS) {
      atomic_guess(basis,sol.C,sol.E,set);
    } else if(guess == MOLGUESS) {
      // Need to generate the starting guess.
      std::string name;
      molecular_guess(basis,set,name);

      // Load guess orbitals
      {
	Checkpoint guesschk(name,false);
	guesschk.read("C",sol.C);
	guesschk.read("E",sol.E);
      }
      // and remove the temporary file
      remove(name.c_str());
    }

    // Get orbital occupancies
    std::vector<double> occs=get_restricted_occupancy(set,basis);

    // Write checkpoint.
    Checkpoint chkpt(savename,true);
    chkpt.write(basis);

    // Write number of electrons
    int Nel_alpha;
    int Nel_beta;
    get_Nel_alpha_beta(basis.Ztot()-set.get_int("Charge"),set.get_int("Multiplicity"),Nel_alpha,Nel_beta);
    chkpt.write("Nel",Nel);
    chkpt.write("Nel-a",Nel_alpha);
    chkpt.write("Nel-b",Nel_beta);

    // Write OMOs
    if(doCW) {
      if(!oldrestr)
	fprintf(stderr,"Projection of OMO matrix between restricted and unrestricted calculations is not supported.\n");
      else
	chkpt.cwrite("CW",CW);
    }

    // Write method
    chkpt.write("Method",set.get_string("Method"));

    // Solver
    SCF solver(basis,set,chkpt);

    // Core guess?
    if(guess==COREGUESS)
      solver.core_guess(sol);
    else if(guess==GWHGUESS)
      solver.gwh_guess(sol);

    // Form density matrix
    sol.P=form_density(sol.C,occs);

    // Freeze core orbitals?
    if(freezecore) {
      // Localize the core orbitals within the occupied space
      size_t nloc=localize_core(basis,std::max(Nel_alpha,Nel_beta),sol.C,verbose);
      // and freeze them
      solver.set_frozen(sol.C.cols(0,nloc-1),0);
    }

    if(hf || rohf) {
      // Solve restricted Hartree-Fock
      solver.do_force(force);
      solver.RHF(sol,occs,conv);
    } else {
      // Print information about used functionals
      if(verbose) {
	print_info(dft.x_func,dft.c_func);
	if(dft.nl)
	  printf("Using VV10 non-local correlation with b = % f, C = %f\n",dft.vv10_b,dft.vv10_C);
      }

      // Perdew-Zunger?
      enum pzrun pz=parse_pzrun(set.get_string("PZ"));
      enum pzham pzh=parse_pzham(set.get_string("PZHam"));
      int pzstab=set.get_int("PZstab");
      std::string pzstabfz=set.get_string("PZstabFz");

      if(pz==NO) {
	if(dft.adaptive) {
	  // Solve restricted DFT problem first on a rough grid
	  solver.RDFT(sol,occs,initconv,initdft);

	  if(verbose) {
	    fprintf(stderr,"\n");
	    fflush(stderr);
	  }
	}

	// ... and then on the more accurate grid
	solver.do_force(force);
	solver.RDFT(sol,occs,conv,dft);

      } else {
	// Run Perdew-Zunger calculation.
	rscf_t oldsol(sol);
	sol.P.zeros();

	// PZ weight
	double pzcor=set.get_double("PZw");
	// Run mode
	pzmet_t pzmet=parse_pzmet(set.get_string("PZmode"));

	// Localization?
	bool pzloc=set.get_bool("PZloc");
	// Seed
	int seed=set.get_int("PZseed");
	// Convergence thresholds
	double thr_Kmax=set.get_double("PZKmax");
	double thr_Krms=set.get_double("PZKrms");
	double thr_Emax=set.get_double("PZEmax");
	double thr_dPmax=set.get_double("PZdPmax");
	double thr_dPrms=set.get_double("PZdPrms");
	double thr_dEmax=set.get_double("PZdEmax");

	size_t pznmax=set.get_int("PZunit");
	int pzniter=set.get_int("PZiter");

	if(pz==CANPERT || pz==REALPERT) { // Perturbative treatment

	  if(dft.adaptive) {
	    // Solve restricted DFT problem first on a rough grid
	    solver.RDFT(sol,occs,initconv,initdft);

	    if(verbose) {
	      fprintf(stderr,"\n");
	      fflush(stderr);
	    }
	  }

	  // ... and then on the more accurate grid
	  solver.RDFT(sol,occs,conv,dft);

	  // DFT grid
	  DFTGrid grid(&basis,verbose,dft.lobatto);
	  DFTGrid nlgrid(&basis,verbose,dft.lobatto);
	  if(!dft.adaptive) {
	    // Fixed size grid
	    grid.construct(dft.nrad,dft.lmax,dft.x_func,dft.c_func,strictint);
	    if(dft.nl)
	      nlgrid.construct(dft.nlnrad,dft.nllmax,true,false,strictint,true);
	  }

	  // Get SIC potential
	  solver.PZSIC_RDFT(sol,occs,dft,pzmet,pzh,pzcor,grid,nlgrid,thr_Emax,thr_Kmax,thr_Krms,pznmax,(pz==CAN || pz==CANPERT),pzloc,(pz==REAL || pz==REALPERT),seed);

	  // Perturbative calculation - no need for self-consistency
	  // Diagonalize to get new orbitals and energies
	  diagonalize(solver.get_S(),solver.get_Sinvh(),sol);

	  // and update density matrices
	  sol.P=form_density(sol.C,occs);

	} else { // Self-consistent treatment

	  if(verbose)
	    printf("\nRunning SIC cycle until energy converged to %e and density to %e max, %e rms.\n\n",thr_dEmax,thr_dPmax,thr_dPrms);

	  // Iteration number
	  int pziter=0;

	  Timer tsic;

	  // Real canonical CMOs?
	  if(pz==OLDFULL)
	    solver.set_real_cmos(true);
	  else
	    solver.set_real_cmos(false);

	  if(dft.adaptive) {
	    while(true) {
	      // Change reference values
	      oldsol=sol;

	      // DFT grid
	      DFTGrid grid(&basis,verbose,dft.lobatto);
	      DFTGrid nlgrid(&basis,verbose,dft.lobatto);
	      if(dft.nl) {
		ERROR_INFO();
		throw std::runtime_error("Should not end up here since adaptive grids are not supported with VV10.\n");
	      }

	      // Get new SIC potential
	      solver.PZSIC_RDFT(sol,occs,initdft,pzmet,pzh,pzcor,grid,nlgrid,thr_Emax,thr_Kmax,thr_Krms,pznmax,(pz==CAN || pz==CANPERT),pzloc,(pz==REAL || pz==REALPERT),seed);
	      pziter++;

	      // Solve self-consistent field equations in presence of new SIC potential
	      solver.RDFT(sol,occs,conv,initdft);

	      // Energy difference
	      double dE=sol.en.E-oldsol.en.E;
	      // Density differences
	      double dP_rms=rms_norm((sol.P-oldsol.P)/2.0);
	      double dP_max=max_abs((sol.P-oldsol.P)/2.0);

	      // Print out changes
	      if(verbose) {
		fprintf(stderr,"%4i % 16.8f",pziter,sol.en.E);

		if(fabs(dE)<thr_dEmax)
		  fprintf(stderr," % 10.3e*",dE);
		else
		  fprintf(stderr," % 10.3e ",dE);

		if(dP_rms<thr_dPrms)
		  fprintf(stderr," %9.3e*",dP_rms);
		else
		  fprintf(stderr," %9.3e ",dP_rms);

		if(dP_max<thr_dPmax)
		  fprintf(stderr," %9.3e*",dP_max);
		else
		  fprintf(stderr," %9.3e ",dP_max);

		fprintf(stderr,"\n");

		printf("\n%7s %13s %12s %12s\n","Errors:","Energy","Max dens","RMS dens");
		printf("%7s % e %e %e\n","",dE,dP_max,dP_rms);
	      }

	      if(fabs(dE)<thr_dEmax && dP_rms<thr_dPrms && dP_max<thr_dPmax)
		break;
	      if(pziter>=pzniter)
		break;
	    }
	    pziter=0;
	  }

	  if(pzniter)
	    while(true) {
	      // Change reference values
	      oldsol=sol;

	      // DFT grid
	      DFTGrid grid(&basis,verbose,dft.lobatto);
	      DFTGrid nlgrid(&basis,verbose,dft.lobatto);
	      if(!dft.adaptive) {
		// Fixed size grid
		grid.construct(dft.nrad,dft.lmax,dft.x_func,dft.c_func,strictint);
		if(dft.nl)
		  nlgrid.construct(dft.nlnrad,dft.nllmax,true,false,strictint,true);
	      }

	      // Get new SIC potential
	      solver.PZSIC_RDFT(sol,occs,dft,pzmet,pzh,pzcor,grid,nlgrid,thr_Emax,thr_Kmax,thr_Krms,pznmax,(pz==CAN || pz==CANPERT),pzloc,(pz==REAL || pz==REALPERT),seed);
	      pziter++;

	      // Solve self-consistent field equations in presence of new SIC potential
	      solver.RDFT(sol,occs,conv,dft);

	      // Energy difference
	      double dE=sol.en.E-oldsol.en.E;
	      // Density differences
	      double dP_rms=rms_norm((sol.P-oldsol.P)/2.0);
	      double dP_max=max_abs((sol.P-oldsol.P)/2.0);

	      // Print out changes
	      if(verbose) {
		fprintf(stderr,"%4i % 16.8f",pziter,sol.en.E);

		if(fabs(dE)<thr_dEmax)
		  fprintf(stderr," % 10.3e*",dE);
		else
		  fprintf(stderr," % 10.3e ",dE);

		if(dP_rms<thr_dPrms)
		  fprintf(stderr," %9.3e*",dP_rms);
		else
		  fprintf(stderr," %9.3e ",dP_rms);

		if(dP_max<thr_dPmax)
		  fprintf(stderr," %9.3e*",dP_max);
		else
		  fprintf(stderr," %9.3e ",dP_max);

		fprintf(stderr,"\n");

		printf("\n%7s %13s %12s %12s\n","Errors:","Energy","Max dens","RMS dens");
		printf("%7s % e %e %e\n","",dE,dP_max,dP_rms);
	      }

	      if(fabs(dE)<thr_dEmax && dP_rms<thr_dPrms && dP_max<thr_dPmax)
		break;
	      if(pziter==pzniter)
		break;
	    }

	  if(verbose)
	    fprintf(stderr,"\nSIC self-consistency solved in %s.\n",tsic.elapsed().c_str());

	  // Stability analysis
	  if(pzstab) {
	    PZStability stab(&solver,dft);

	    arma::uvec drop;
	    if(pzstabfz.size()) {
	      // Convert to C++ indexing
	      drop=arma::conv_to<arma::uvec>::from(parse_range(splitline(pzstabfz)[0]))-1;
	    }

	    /*
	    // Optimize
	    stab.set(sol,pz!=REAL,true,true);
	    stab.optimize();
	    */

	    stab.set(sol,drop,true,abs(pzstab)==2);
	    stab.check();
	  }
	}
      }

      // and update checkpoint file entries
      chkpt.write("C",sol.C);
      chkpt.write("E",sol.E);
      chkpt.write("P",sol.P);
      chkpt.write(sol.en);

      // Do we need forces?
      if(force) {
	solver.do_force(true);
	solver.RDFT(sol,occs,conv,dft);
      }
    }

    // Do population analysis
    if(verbose) {
      population_analysis(basis,sol.P);
    }

  } else {
    uscf_t sol;
    // Initialize energy
    memset(&sol.en, 0, sizeof(energy_t));

    if(doload) {
      // Running polarized calculation but given restricted guess
      if(oldrestr) {
	// Project solution to new basis
	basis.projectMOs(oldbas,Eold,Cold,sol.Ea,sol.Ca);
	sol.Eb=sol.Ea;
	sol.Cb=sol.Ca;
      } else {
	// Project to new basis.
	basis.projectMOs(oldbas,Eaold,Caold,sol.Ea,sol.Ca);
	basis.projectMOs(oldbas,Ebold,Cbold,sol.Eb,sol.Cb);
      }
    } else if(guess == ATOMGUESS) {
      atomic_guess(basis,sol.Ca,sol.Ea,set);
      sol.Cb=sol.Ca;
      sol.Eb=sol.Ea;
    } else if(guess == MOLGUESS) {
      // Need to generate the starting guess.
      std::string name;
      molecular_guess(basis,set,name);

      // Load guess orbitals
      {
	Checkpoint guesschk(name,false);
	guesschk.read("Ca",sol.Ca);
	guesschk.read("Ea",sol.Ea);
	guesschk.read("Cb",sol.Cb);
	guesschk.read("Eb",sol.Eb);
      }
      // and remove the temporary file
      remove(name.c_str());
    }

    // Get orbital occupancies
    std::vector<double> occa, occb;
    get_unrestricted_occupancy(set,basis,occa,occb);

    // Write checkpoint.
    Checkpoint chkpt(savename,true);
    chkpt.write(basis);

    // Write number of electrons
    int Nel_alpha;
    int Nel_beta;
    get_Nel_alpha_beta(basis.Ztot()-set.get_int("Charge"),set.get_int("Multiplicity"),Nel_alpha,Nel_beta);
    chkpt.write("Nel",Nel);
    chkpt.write("Nel-a",Nel_alpha);
    chkpt.write("Nel-b",Nel_beta);

    // Write OMOs
    if(doCW) {
      if(oldrestr)
	fprintf(stderr,"Projection of OMO matrix between restricted and unrestricted calculations is not supported.\n");
      else {
	chkpt.cwrite("CWa",CWa);
	chkpt.cwrite("CWb",CWb);
      }
    }

    // Solver
    SCF solver(basis,set,chkpt);

    // Core guess?
    if(guess==COREGUESS)
      solver.core_guess(sol);
    else if(guess==GWHGUESS)
      solver.gwh_guess(sol);
    // Form density matrix
    sol.Pa=form_density(sol.Ca,occa);
    sol.Pb=form_density(sol.Cb,occb);
    sol.P=sol.Pa+sol.Pb;

    // Freeze core orbitals?
    if(freezecore) {
      // Get the natural orbitals
      arma::mat natorb;
      arma::vec occs;
      form_NOs(sol.P,basis.overlap(),natorb,occs);

      // Then, localize the core orbitals within the occupied space
      size_t nloc=localize_core(basis,std::max(Nel_alpha,Nel_beta),natorb);
      // and freeze them
      solver.set_frozen(natorb.cols(0,nloc-1),0);
      // Update the current orbitals as well
      sol.Ca=natorb;
      sol.Cb=natorb;
    }

    if(hf) {
      // Solve restricted Hartree-Fock
      solver.do_force(force);
      solver.UHF(sol,occa,occb,conv);
    } else if(rohf) {
      // Solve restricted open-shell Hartree-Fock

      // Solve ROHF
      solver.ROHF(sol,occa,occb,conv);

      // Set occupancies right
      get_unrestricted_occupancy(set,basis,occa,occb);
    } else {
      // Print information about used functionals
      if(verbose) {
	print_info(dft.x_func,dft.c_func);
	if(dft.nl)
	  printf("Using VV10 non-local correlation with b = % f, C = %f\n",dft.vv10_b,dft.vv10_C);
      }

      // Perdew-Zunger?
      enum pzrun pz=parse_pzrun(set.get_string("PZ"));
      enum pzham pzh=parse_pzham(set.get_string("PZHam"));

      int pzstab=set.get_int("PZstab");
      std::string pzstabfz=set.get_string("PZstabFz");

      if(pz==NO) {
	if(dft.adaptive) {
	  // Solve unrestricted DFT problem first on a rough grid
	  solver.UDFT(sol,occa,occb,initconv,initdft);

	  if(verbose) {
	    fprintf(stderr,"\n");
	    fflush(stderr);
	  }
	}
	// ... and then on the more accurate grid
	solver.do_force(force);
	solver.UDFT(sol,occa,occb,conv,dft);

      } else {
	// PZ weight
	double pzcor=set.get_double("PZw");
	// Run mode
	pzmet_t pzmet=parse_pzmet(set.get_string("PZmode"));
	// Localization?
	bool pzloc=set.get_bool("PZloc");
	// Seed
	int seed=set.get_int("PZseed");

	// Convergence thresholds
	double thr_Kmax=set.get_double("PZKmax");
	double thr_Krms=set.get_double("PZKrms");
	double thr_Emax=set.get_double("PZEmax");
	double thr_dPmax=set.get_double("PZdPmax");
	double thr_dPrms=set.get_double("PZdPrms");
	double thr_dEmax=set.get_double("PZdEmax");
	int pzunit=set.get_int("PZunit");
	int pzniter=set.get_int("PZiter");

	Timer tsic;

	if(pz==CANPERT || pz==REALPERT) {

	  if(dft.adaptive) {
	    // Solve restricted DFT problem first on a rough grid
	    solver.UDFT(sol,occa,occb,initconv,initdft);

	    if(verbose) {
	      fprintf(stderr,"\n");
	      fflush(stderr);
	    }
	  }

	  // ... and then on the more accurate grid
	  solver.UDFT(sol,occa,occb,conv,dft);

	  // DFT grid
	  DFTGrid grid(&basis,verbose,dft.lobatto);
	  DFTGrid nlgrid(&basis,verbose,dft.lobatto);
	  if(!dft.adaptive) {
	    // Fixed size grid
	    grid.construct(dft.nrad,dft.lmax,dft.x_func,dft.c_func,strictint);
	    if(dft.nl)
	      nlgrid.construct(dft.nlnrad,dft.nllmax,true,false,strictint,true);
	  }

	  // Get SIC potential
	  solver.PZSIC_UDFT(sol,occa,occb,dft,pzmet,pzh,pzcor,grid,nlgrid,thr_Emax,thr_Kmax,thr_Krms,pzunit,(pz==CAN || pz==CANPERT),pzloc,(pz==REAL || pz==REALPERT),seed);

          // Perturbative calculation - no need for self-consistency
    	  // Diagonalize to get new orbitals and energies
	  diagonalize(solver.get_S(),solver.get_Sinvh(),sol);
	  // update density matrices
	  sol.Pa=form_density(sol.Ca,occa);
	  sol.Pb=form_density(sol.Cb,occb);
	  sol.P=sol.Pa+sol.Pb;

	} else {
	  if(verbose)
	    printf("\nRunning SIC cycle until energy converged to %e and density to %e max, %e rms.\n\n",thr_dEmax,thr_dPmax,thr_dPrms);

	  // Real canonical CMOs?
	  if(pz==OLDFULL)
	    solver.set_real_cmos(true);
	  else
	    solver.set_real_cmos(false);

	  // Solution to last iteration
	  uscf_t oldsol;

	  // Iteration number
	  int pziter=0;

	  if(dft.adaptive) {
	    while(true) {
	      // Change reference values
	      oldsol=sol;

	      // DFT grid
	      DFTGrid grid(&basis,verbose,dft.lobatto);
	      DFTGrid nlgrid(&basis,verbose,dft.lobatto);
	      if(!dft.adaptive) {
		// Fixed size grid
		grid.construct(initdft.nrad,initdft.lmax,initdft.x_func,initdft.c_func,strictint);
		if(dft.nl)
		  nlgrid.construct(initdft.nlnrad,initdft.nllmax,true,false,strictint,true);
	      }

	      // Get new SIC potential
	      solver.PZSIC_UDFT(sol,occa,occb,initdft,pzmet,pzh,pzcor,grid,nlgrid,thr_dEmax,thr_Kmax,thr_Krms,pzunit,(pz==CAN || pz==CANPERT),pzloc,(pz==REAL || pz==REALPERT),seed);
	      pziter++;

	      // Solve self-consistent field equations in presence of new SIC potential
	      solver.UDFT(sol,occa,occb,conv,dft);

	      // Energy difference
	      double dE=sol.en.E-oldsol.en.E;
	      // Density differences
	      double dPa_rms=rms_norm(sol.Pa-oldsol.Pa);
	      double dPa_max=max_abs(sol.Pa-oldsol.Pa);
	      double dPb_rms=rms_norm(sol.Pb-oldsol.Pb);
	      double dPb_max=max_abs(sol.Pb-oldsol.Pb);
	      double dP_rms=std::max(dPa_rms,dPb_rms);
	      double dP_max=std::max(dPa_max,dPb_max);

	      // Print out changes
	      if(verbose) {
		fprintf(stderr,"%4i % 16.8f",pziter,sol.en.E);

		if(fabs(dE)<thr_dEmax)
		  fprintf(stderr," % 10.3e*",dE);
		else
		  fprintf(stderr," % 10.3e ",dE);

		if(dP_rms<thr_dPrms)
		  fprintf(stderr," %9.3e*",dP_rms);
		else
		  fprintf(stderr," %9.3e ",dP_rms);

		if(dP_max<thr_dPmax)
		  fprintf(stderr," %9.3e*",dP_max);
		else
		  fprintf(stderr," %9.3e ",dP_max);

		fprintf(stderr,"\n");

		printf("\n%7s %13s %12s %12s\n","Errors:","Energy","Max dens","RMS dens");
		printf("%7s % e %e %e\n","",dE,dP_max,dP_rms);
		printf("%7s %13s %e %e\n","alpha","",dPa_max,dPa_rms);
		printf("%7s %13s %e %e\n","beta","",dPb_max,dPb_rms);
	      }

	      if(fabs(dE)<thr_dEmax && std::max(dPa_rms,dPb_rms)<thr_dPrms && std::max(dPa_max,dPb_max)<thr_dPmax)
		break;
	      if(pziter==pzniter)
		break;
	    }
	    pziter=0;
	  }

	  while(true) {
	    // Change reference values
	    oldsol=sol;

	    // DFT grid
	    DFTGrid grid(&basis,verbose,dft.lobatto);
	    DFTGrid nlgrid(&basis,verbose,dft.lobatto);
	    if(!dft.adaptive) {
	      // Fixed size grid
	      grid.construct(dft.nrad,dft.lmax,dft.x_func,dft.c_func,strictint);
	      if(dft.nl)
		nlgrid.construct(dft.nlnrad,dft.nllmax,true,false,strictint,true);
	    }

	    // Get new SIC potential
	    solver.PZSIC_UDFT(sol,occa,occb,dft,pzmet,pzh,pzcor,grid,nlgrid,thr_dEmax,thr_Kmax,thr_Krms,pzunit,(pz==CAN || pz==CANPERT),pzloc,(pz==REAL || pz==REALPERT),seed);
	    pziter++;

	    // Solve self-consistent field equations in presence of new SIC potential
	    solver.UDFT(sol,occa,occb,conv,dft);

	    // Energy difference
	    double dE=sol.en.E-oldsol.en.E;
	    // Density differences
	    double dPa_rms=rms_norm(sol.Pa-oldsol.Pa);
	    double dPa_max=max_abs(sol.Pa-oldsol.Pa);
	    double dPb_rms=rms_norm(sol.Pb-oldsol.Pb);
	    double dPb_max=max_abs(sol.Pb-oldsol.Pb);
	    double dP_rms=rms_norm(sol.P-oldsol.P);
	    double dP_max=max_abs(sol.P-oldsol.P);

	    // Print out changes
	    if(verbose) {
	      fprintf(stderr,"%4i % 16.8f",pziter,sol.en.E);

	      if(fabs(dE)<thr_dEmax)
		fprintf(stderr," % 10.3e*",dE);
	      else
		fprintf(stderr," % 10.3e ",dE);

	      if(dP_rms<thr_dPrms)
		fprintf(stderr," %9.3e*",dP_rms);
	      else
		fprintf(stderr," %9.3e ",dP_rms);

	      if(dP_max<thr_dPmax)
		fprintf(stderr," %9.3e*",dP_max);
	      else
		fprintf(stderr," %9.3e ",dP_max);

	      fprintf(stderr,"\n");

	      printf("\n%7s %13s %12s %12s\n","Errors:","Energy","Max dens","RMS dens");
	      printf("%7s % e %e %e\n","",dE,dP_max,dP_rms);
	      printf("%7s %13s %e %e\n","alpha","",dPa_max,dPa_rms);
	      printf("%7s %13s %e %e\n","beta","",dPb_max,dPb_rms);
	    }

	    if(fabs(dE)<thr_dEmax && std::max(dPa_rms,dPb_rms)<thr_dPrms && std::max(dPa_max,dPb_max)<thr_dPmax)
	      break;
	    if(pziter==pzniter)
	      break;
	  }

	  if(verbose)
	    fprintf(stderr,"\nSIC self-consistency solved in %s.\n",tsic.elapsed().c_str());

	  // Stability analysis
	  if(pzstab) {
	    PZStability stab(&solver,dft);

	    arma::uvec dropa, dropb;
	    if(pzstabfz.size()) {
	      // Get ranges and convert to C++ indexing
	      dropa=arma::conv_to<arma::uvec>::from(parse_range(splitline(pzstabfz)[0]))-1;
	      dropb=arma::conv_to<arma::uvec>::from(parse_range(splitline(pzstabfz)[1]))-1;
	    }
	    /*
	    // Optimize
	    stab.set(sol,pz!=REAL,true,true);
	    stab.optimize();
	    */

	    stab.set(sol,dropa,dropb,true,abs(pzstab)==2);
	    stab.check();
	  }
	}

	// and update checkpoint file entries
	chkpt.write("Ca",sol.Ca);
	chkpt.write("Cb",sol.Cb);
	chkpt.write("Ea",sol.Ea);
	chkpt.write("Eb",sol.Eb);
	chkpt.write("Pa",sol.Pa);
	chkpt.write("Pb",sol.Pb);
	chkpt.write("P",sol.P);
	chkpt.write(sol.en);

	// Do we need forces?
	if(force) {
	  solver.do_force(true);
	  solver.UDFT(sol,occa,occb,conv,dft);
	}

      }
    }

    if(verbose) {
      population_analysis(basis,sol.Pa,sol.Pb);
    }
  }
}

bool operator<(const ovl_sort_t & lhs, const ovl_sort_t & rhs) {
  // Sort into decreasing order
  return lhs.S > rhs.S;
}

arma::mat project_orbitals(const arma::mat & Cold, const BasisSet & minbas, const BasisSet & augbas) {
  Timer ttot;
  Timer t;

  // Total number of functions in augmented set is
  const size_t Ntot=augbas.get_Nbf();
  // Amount of old orbitals is
  const size_t Nold=Cold.n_cols;

  // Identify augmentation shells.
  std::vector<size_t> augshellidx;
  std::vector<size_t> origshellidx;

  std::vector<GaussianShell> augshells=augbas.get_shells();
  std::vector<GaussianShell> origshells=minbas.get_shells();

  // Loop over shells in augmented set.
  for(size_t i=0;i<augshells.size();i++) {
    // Try to find the shell in the original set
    bool found=false;
    for(size_t j=0;j<origshells.size();j++)
      if(augshells[i]==origshells[j]) {
	found=true;
	origshellidx.push_back(i);
	break;
      }

    // If the shell was not found in the original set, it is an
    // augmentation shell.
    if(!found)
      augshellidx.push_back(i);
  }

  // Overlap matrix in augmented basis
  arma::mat S=augbas.overlap();
  arma::vec Sval;
  arma::mat Svec;
  eig_sym_ordered(Sval,Svec,S);

  printf("Condition number of overlap matrix is %e.\n",Sval(0)/Sval(Sval.n_elem-1));

  printf("Diagonalization of basis took %s.\n",t.elapsed().c_str());
  t.set();

  // Count number of independent functions
  size_t Nind=0;
  for(size_t i=0;i<Ntot;i++)
    if(Sval(i)>=LINTHRES)
      Nind++;

  printf("Augmented basis has %i linearly independent and %i dependent functions.\n",(int) Nind,(int) (Ntot-Nind));

  // Drop linearly dependent ones.
  Sval=Sval.subvec(Sval.n_elem-Nind,Sval.n_elem-1);
  Svec=Svec.cols(Svec.n_cols-Nind,Svec.n_cols-1);

  // Form Sinvh
  arma::mat Sinvh(Ntot,Nind);
  for(size_t i=0;i<Nind;i++)
    Sinvh.col(i)=Svec.col(i)/sqrt(Sval(i));

  // Form the new C matrix.
  arma::mat C(Ntot,Nind);
  C.zeros();

  // The first vectors are simply the occupied states.
  for(size_t i=0;i<Nold;i++)
    for(size_t ish=0;ish<origshellidx.size();ish++)
      C.submat(augshells[origshellidx[ish]].get_first_ind(),i,augshells[origshellidx[ish]].get_last_ind(),i)=Cold.submat(origshells[ish].get_first_ind(),i,origshells[ish].get_last_ind(),i);

  // Determine the rest. Compute the overlap of the functions
  arma::mat X=arma::trans(Sinvh)*S*C.cols(0,Nold-1);
  // and perform SVD
  arma::mat U, V;
  arma::vec s;
  bool svdok=arma::svd(U,s,V,X);
  if(!svdok)
    throw std::runtime_error("SVD decomposition failed!\n");

  // Rotate eigenvectors.
  Sinvh=Sinvh*U;

  // Now, the subspace of the small basis set is found in the first
  // Nmo eigenvectors.
  C.cols(Nold,Nind-1)=Sinvh.cols(Nold,Nind-1);

  try {
    // Check orthogonality of orbitals
    check_orth(C,S,false);
  } catch(std::runtime_error & err) {
    std::ostringstream oss;
    oss << "Projected orbitals are not orthonormal. Please report this bug.";
    throw std::runtime_error(oss.str());
  }

  printf("Projected orbitals in %s.\n",ttot.elapsed().c_str());
  fflush(stdout);

  return C;
}

std::vector<int> symgroups(const arma::mat & C, const arma::mat & S, const std::vector<arma::mat> & freeze, bool verbose) {
  // Initialize groups.
  std::vector<int> gp(C.n_cols,0);

  // Loop over frozen core groups
  for(size_t igp=0;igp<freeze.size();igp++) {

    // Compute overlap of orbitals with frozen core orbitals
    std::vector<ovl_sort_t> ovl(C.n_cols);
    for(size_t i=0;i<C.n_cols;i++) {

      // Store index
      ovl[i].idx=i;
      // Initialize overlap
      ovl[i].S=0.0;

      // Helper vector
      arma::vec hlp=S*C.col(i);

      // Loop over frozen orbitals.
      for(size_t ifz=0;ifz<freeze[igp].n_cols;ifz++) {
	// Compute projection
	double proj=arma::dot(hlp,freeze[igp].col(ifz));
	// Increment overlap
	ovl[i].S+=proj*proj;
      }
    }

    // Sort the projections
    std::sort(ovl.begin(),ovl.end());

    // Store the symmetries
    for(size_t i=0;i<freeze[igp].n_cols;i++) {
      // The orbital with the maximum overlap is
      size_t maxind=ovl[i].idx;
      // Change symmetry of orbital with maximum overlap
      gp[maxind]=igp+1;

      if(verbose)
	printf("Set symmetry of orbital %i to %i (overlap %e).\n",(int) maxind+1,gp[maxind],ovl[i].S);
    }

  }

  return gp;
}

void freeze_orbs(const std::vector<arma::mat> & freeze, const arma::mat & C, const arma::mat & S, arma::mat & H, bool verbose) {
  // Freezes the orbitals corresponding to different symmetry groups.

  // Form H_MO
  arma::mat H_MO=arma::trans(C)*H*C;

  // Get symmetry groups
  std::vector<int> sg=symgroups(C,S,freeze,verbose);

  // Loop over H_MO and zero out elements where symmetry groups differ
  for(size_t i=0;i<H_MO.n_rows;i++)
    for(size_t j=0;j<=i;j++)
      if(sg[i]!=sg[j]) {
	H_MO(i,j)=0;
	H_MO(j,i)=0;
      }

  // Back-transform to AO
  arma::mat SC=S*C;

  H=SC*H_MO*arma::trans(SC);
}

size_t localize_core(const BasisSet & basis, int nocc, arma::mat & C, bool verbose) {
  // Check orthonormality
  arma::mat S=basis.overlap();
  check_orth(C,S,false);

  const int Nmagic=(int) (sizeof(magicno)/sizeof(magicno[0]));

  // First, figure out how many orbitals to localize on each center
  std::vector<size_t> locno(basis.get_Nnuc(),0);
  // Localize on all the atoms of the same type than the excited atom
  for(size_t i=0;i<basis.get_Nnuc();i++)
    if(!basis.get_nucleus(i).bsse) {
      // Charge of nucleus is
      int Z=basis.get_nucleus(i).Z;

      // Get the number of closed shells
      int ncl=0;
      for(int j=0;j<Nmagic-1;j++)
	if(magicno[j] <= Z && Z <= magicno[j+1]) {
	  ncl=magicno[j]/2;
	  break;
	}

      // Store number of closed shells
      locno[i]=ncl;
    } else
      locno[i]=0;

  // Amount of orbitals already localized
  size_t locd=0;
  // Perform the localization.
  for(size_t inuc=0;inuc<locno.size();inuc++) {
    if(locno[inuc]==0)
      continue;

    // The nucleus is located at
    coords_t cen=basis.get_nuclear_coords(inuc);

    // Compute moment integrals around the nucleus
    std::vector<arma::mat> momstack=basis.moment(2,cen.x,cen.y,cen.z);
    // Get matrix which transforms into occupied MO basis
    arma::mat transmat=C.cols(locd,nocc-1);

    // Sum together to get x^2 + y^2 + z^2
    arma::mat rsqmat=momstack[getind(2,0,0)]+momstack[getind(0,2,0)]+momstack[getind(0,0,2)];
    // and transform into the occupied MO basis
    rsqmat=arma::trans(transmat)*rsqmat*transmat;

    // Diagonalize rsq_mo
    arma::vec reig;
    arma::mat rvec;
    eig_sym_ordered(reig,rvec,rsqmat);

    /*
      printf("\nLocalization around center %i, eigenvalues (Å):",(int) locind[i].ind+1);
      for(size_t ii=0;ii<reig.n_elem;ii++)
      printf(" %e",sqrt(reig(ii))/ANGSTROMINBOHR);
      printf("\n");
      fflush(stdout);
    */

    // Rotate yet unlocalized orbitals
    C.cols(locd,nocc-1)=transmat*rvec;

    // Increase number of localized orbitals
    locd+=locno[inuc];

    if(verbose)
      for(size_t k=0;k<locno[inuc];k++) {
	printf("Localized orbital around nucleus %i with Rrms=%e Å.\n",(int) inuc+1,sqrt(reig(k))/ANGSTROMINBOHR);
	fflush(stdout);
      }
  }

  // Check orthonormality
  check_orth(C,S,false);

  return locd;
}

arma::mat interpret_force(const arma::vec & f) {
  if(f.n_elem%3!=0) {
    ERROR_INFO();
    std::ostringstream oss;
    oss << "Error in intepret_force: expecting a vector containing forces, but given vector has " << f.n_elem << " elements!\n";
    throw std::runtime_error(oss.str());
  }

  arma::mat force(f);
  force.reshape(3,f.n_elem/3);
  return force;
}
