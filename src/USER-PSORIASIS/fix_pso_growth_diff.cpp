/* ----------------------------------------------------------------------
 PSORIASIS package - Contributing authors: Dinika P.

 NUFEB package - A LAMMPS user package for Individual-based Modelling of Microbial Communities
 Contributing authors: Bowen Li & Denis Taniguchi (Newcastle University, UK)
 Email: bowen.li2@newcastle.ac.uk & denis.taniguchi@newcastle.ac.uk


 Copyright (2003) Sandia Corporation.  Under the terms of Contract
 DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
 certain rights in this software.  This software is distributed under
 the GNU General Public License.

 See the README file in the top-level LAMMPS directory.
 ------------------------------------------------------------------------- */

#include "fix_pso_growth_diff.h"

#include <math.h>
#include <string.h>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <vector>

#include "atom.h"
#include "atom_vec_bio.h"
#include "domain.h"
#include "error.h"
#include "force.h"
#include "input.h"
#include "lammps.h"
#include "math_const.h"
#include "memory.h"

#include "bio.h"
#include "fix_bio_kinetics.h"
#include "modify.h"
#include "pointers.h"
#include "update.h"
#include "variable.h"
#include "group.h"

using namespace LAMMPS_NS;
using namespace FixConst;
using namespace MathConst;
using namespace std;


/* ---------------------------------------------------------------------- */

FixPGrowthDIFF::FixPGrowthDIFF(LAMMPS *lmp, int narg, char **arg) :
	Fix(lmp, narg, arg) {
  avec = (AtomVecBio *) atom->style_match("bio");
  if (!avec)
	error->all(FLERR, "Fix psoriasis/growth/diff requires atom style bio");

  if (narg < 7)
	error->all(FLERR, "Not enough arguments in fix psoriasis/growth/diff command");

  varg = narg-3;
  var = new char*[varg];
  ivar = new int[varg];

  for (int i = 0; i < varg; i++) {
	int n = strlen(&arg[3 + i][2]) + 1;
	var[i] = new char[n];
	strcpy(var[i], &arg[3 + i][2]);
  }

  kinetics = NULL;

  external_gflag = 1;

  int iarg = 7;
  while (iarg < narg){
	if (strcmp(arg[iarg],"gflag") == 0) {
	  external_gflag = force->inumeric(FLERR, arg[iarg+1]);
	  if (external_gflag != 0 && external_gflag != 1)
		error->all(FLERR, "Illegal fix psoriasis/growth/diff command: gflag");
	  iarg += 2;
	} else
	  error->all(FLERR, "Illegal fix psoriasis/growth/diff command");
  }
}

/* ---------------------------------------------------------------------- */

FixPGrowthDIFF::~FixPGrowthDIFF() {
  int i;
  for (i = 0; i < varg; i++) {
	delete[] var[i];
  }
  delete[] var;
  delete[] ivar;

  memory->destroy(species);
}

/* ---------------------------------------------------------------------- */

int FixPGrowthDIFF::setmask() {
  int mask = 0;
  mask |= PRE_FORCE;
  return mask;
}

/* ----------------------------------------------------------------------
 if need to restore per-atom quantities, create new fix STORE styles
 ------------------------------------------------------------------------- */

void FixPGrowthDIFF::init() {
  if (!atom->radius_flag)
	error->all(FLERR, "Fix requires atom attribute diameter");

  for (int n = 0; n < varg; n++) {
	ivar[n] = input->variable->find(var[n]);
	if (ivar[n] < 0)
	  error->all(FLERR, "Variable name for fix psoriasis/growth/diff does not exist");
	if (!input->variable->equalstyle(ivar[n]))
	  error->all(FLERR, "Variable for fix psoriasis/growth/diff is invalid style");
  }

  //modify atom mask
    for (int i = 1; i < group->ngroup; i++) {
      if (strcmp(group->names[i],"CC") == 0) {
        cc_mask = pow(2, i) + 1;
        break;
      }
    }

  // register fix kinetics with this class
  kinetics = NULL;

  int nfix = modify->nfix;
  for (int j = 0; j < nfix; j++) {
	if (strcmp(modify->fix[j]->style, "kinetics") == 0) {
	  kinetics = static_cast<FixKinetics *>(lmp->modify->fix[j]);
	  break;
	}
  }

  if (kinetics == NULL)
	lmp->error->all(FLERR, "fix kinetics command is required for running IbM simulation");

  diff_dens = input->variable->compute_equal(ivar[0]);
  apop = input->variable->compute_equal(ivar[1]);
  decay_cc = input->variable->compute_equal(ivar[2]);
  ddesq = input->variable->compute_equal(ivar[3]);

  bio = kinetics->bio;

  if (bio->nnu == 0)
	error->all(FLERR, "fix_psoriasis/growth/diff requires Nutrients input");
  else if (bio->decay == NULL)
	error->all(FLERR, "fix_psoriasis/growth/diff requires Decay input");
  else if (bio->mu == NULL)
	error->all(FLERR, "fix_psoriasis/growth/diff requires Growth Rate input");
  else if (bio->ks == NULL)
      error->all(FLERR, "fix_psoriasis/growth/diff requires Ks input");
  else if (bio->yield == NULL)
        error->all(FLERR, "fix_psoriasis/growth/diff requires Yield input");

  nx = kinetics->nx;
  ny = kinetics->ny;
  nz = kinetics->nz;

  species = memory->create(species, atom->ntypes+1, "diff:species");

  //Get computational domain size
  if (domain->triclinic == 0) {
	xlo = domain->boxlo[0];
	xhi = domain->boxhi[0];
	ylo = domain->boxlo[1];
	yhi = domain->boxhi[1];
	zlo = domain->boxlo[2];
	zhi = domain->boxhi[2];
  } else {
	xlo = domain->boxlo_bound[0];
	xhi = domain->boxhi_bound[0];
	ylo = domain->boxlo_bound[1];
	yhi = domain->boxhi_bound[1];
	zlo = domain->boxlo_bound[2];
	zhi = domain->boxhi_bound[2];
  }

  stepx = (xhi - xlo) / nx;
  stepy = (yhi - ylo) / ny;
  stepz = (zhi - zlo) / nz;

  vol = stepx * stepy * stepz;

  init_param();

}

/* ---------------------------------------------------------------------- */

void FixPGrowthDIFF::init_param() {
	ca = 0;

  // initialize nutrient
  for (int nu = 1; nu <= bio->nnu; nu++) {
	if (strcmp(bio->nuname[nu], "ca") == 0)
			ca = nu;
  }

  if (ca == 0)
         	error->all(FLERR, "fix_psoriasis/growth/sc requires nutrient ca");

  //initialise type
  for (int i = 1; i <= atom->ntypes; i++) {
	  char *name = bio->tname[i];

	  if (strcmp(name, "stem") == 0)
		species[i] = 1;
	  else if (strcmp(name, "ta") == 0)
		species[i] = 2;
	  else if (strcmp(name, "diff") == 0)
		species[i] = 3;
	  else if (strcmp(name, "tcell") == 0)
		species[i] = 4;
	  else if (strcmp(name, "cc") == 0)
		species[i] = 5;
	  else if (strcmp(name, "apop") == 0)
		  species[i] = 6;
	  else if (strcmp(name, "bm") == 0)
	  	species[i] = 7;
	  else
		error->all(FLERR, "unknown species in fix_psoriasis/growth/diff");

  }
}


/* ----------------------------------------------------------------------
 metabolism and atom update
 for each cell type -> growth and decay rates are used (mu[i], decay[i])
 nutrient reaction rate is then calculated

 todo: place update biomass in here since we are calculating based on cell rather than grid
 ------------------------------------------------------------------------- */
void FixPGrowthDIFF::growth(double dt, int gflag) {
  int *mask = atom->mask;
  int nlocal = atom->nlocal;
  int *type = atom->type;
  int ntypes = atom->ntypes;

  double *radius = atom->radius;
  double *rmass = atom->rmass;

  double *mu = bio->mu;
  double *decay = bio->decay;
  double *diff_coeff = bio->diff_coeff;
  double **ks = bio->ks;
  double *yield = bio->yield;

  double **xdensity = kinetics->xdensity;
  double **nus = kinetics->nus;
  double **nur = kinetics->nur;

  double growrate_d = 0;

  //different heights for diff cells
//		  double sgheight = zhi * 0.85; //cubic domain
//		  double scheight = zhi * 0.92;
//		  double ssheight = zhi * 0.8;
	  double ssheight = zhi * 0.73; //smaller domain
	  //double sgheight = zhi * 0.85;
	  double sgheight = zhi * 0.7; // JUST FOR TESTING IF DIFF CELL WILL CHANGE TO CC
	  double scheight = zhi * 0.9;

//  for (int i = 0; i < nlocal; i++) {
//  	if (atom->type[i] == 3 && i == 16004 || atom->type[i] == 3 && i == 18180 || atom->type[i] == 3 && i == 19909 || atom->type[i] == 3 && i == 20250) {
//		 //printf("diff cell type %i x: %e y: %e z: %e update_timestep %e age of cell %e \n", atom->type[i], atom->x[i][0], atom->x[i][1], atom->x[i][2], update->ntimestep, update->ntimestep/1001);
//		 //printf("age hour to min %e \n", (update->ntimestep/1001)*60);
//  		printf("type %i   i %i  update_ntimestep %i age %i \n", atom->type[i], i, update->ntimestep, update->ntimestep/1001);
//  		printf("atom coords x %e y %e z %e \n", atom->x[i][0], atom->x[i][1], atom->x[i][2]);
//  	}
//  }
  for (int grid = 0; grid < kinetics->bgrids; grid++) {
	  //grid without atom is not considered
	  if(!xdensity[0][grid]) continue;

	  for (int i = 1; i <= ntypes; i++) {
		  int spec = species[i];

		  //differentiated cells
		  if (spec == 3) {
			  //printf("------- start of growth/diff  -------- \n");
			 //decay rate for diff
			double r8 = decay[i];
			//apoptosis rate
			double r9 = r8 * apop;

			//printf("growrate_diff equation is R8 %e  R9 %e  R10 %e \n", r8, r9, r10);
			//printf("growth_diff grid %i ca %e \n", grid, nus[ca][grid]);

			nur[ca][grid] += (1/yield[i]) * (r8 + r9) *  xdensity[i][grid];
			growrate_d = - (r8 + r9) ;

			//if (atom->x[i][2] > sgheight) { //if in SG layer then secrete out most calcium

		  }

		  // corneocytes
		if (spec == 5) {
			//decay rate for cc
			double r8 = decay_cc;
			//desquamation rate
			double r9 = ddesq;
			//apoptosis rate
			double r10 = (r8 + r9) * apop;

			nur[ca][grid] += (1/yield[i]) * (r8 + r9 + r10) *  xdensity[i][grid];
			growrate_d = - (r8 + r9 + r10);
		}
	  }
  	}
  //update physical attributes
    if (gflag && external_gflag) update_biomass(growrate_d, dt);
}

/* ----------------------------------------------------------------------
 update particle attributes: biomass, outer mass, radius etc
 ------------------------------------------------------------------------- */
void FixPGrowthDIFF::update_biomass(double growrate, double dt) {
  int *mask = atom->mask;
  int nlocal = atom->nlocal;
  int *type = atom->type;

  double *radius = atom->radius;
  double *rmass = atom->rmass;

  const double three_quarters_pi = (3.0 / (4.0 * MY_PI));
  const double four_thirds_pi = 4.0 * MY_PI / 3.0;
  const double third = 1.0 / 3.0;

  double ssheight = zhi * 0.73; //smaller domain
  double sgheight = zhi * 0.85;
  double scheight = zhi * 0.9;
  double dia = 0.6e-5;
  int cc_id = bio->find_typeid("cc");

  for (int i = 0; i < nlocal; i++) {
    if (mask[i] & groupbit) {
      double density = rmass[i] / (four_thirds_pi * radius[i] * radius[i] * radius[i]);

      rmass[i] = rmass[i] * (1 + growrate * dt);
      radius[i] = pow(three_quarters_pi * (rmass[i] / density), third);

		if (radius[i] * 2 < dia){
			//change atom type
			atom->type[i] = cc_id;
			atom->mask[i] = cc_mask;

			printf("enters here");
		}
    }
  }
}

