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

#include "fix_pso_divide_ta.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <random>

#include "atom.h"
#include "atom_vec_bio.h"
#include "domain.h"
#include "error.h"
#include "bio.h"
#include "fix_bio_fluid.h"
#include "force.h"
#include "input.h"
#include "lmptype.h"
#include "math_const.h"
#include "pointers.h"
#include "random_park.h"
#include "update.h"
#include "variable.h"
#include "modify.h"
#include "group.h"

using namespace LAMMPS_NS;
using namespace FixConst;
using namespace MathConst;

#define EPSILON 0.001
#define DELTA 1.005

// enum{PAIR,KSPACE,ATOM};
// enum{DIAMETER,CHARGE};

/* ---------------------------------------------------------------------- */

FixPDivideTa::FixPDivideTa(LAMMPS *lmp, int narg, char **arg) :
    Fix(lmp, narg, arg) {
  avec = (AtomVecBio *) atom->style_match("bio");
  // create instance of nufeb particle (outermass etc)
  if (!avec)
    error->all(FLERR, "Fix kinetics requires atom style bio");
  // check for # of input param
  if (narg < 8)
    error->all(FLERR, "Illegal fix divide command: not enough arguments");
  // read first input param
  nevery = force->inumeric(FLERR, arg[3]);
  if (nevery < 0)
    error->all(FLERR, "Illegal fix divide command: nevery is negative");
  // read 2, 3 input param (variable)
  var = new char*[3];
  ivar = new int[3];

  for (int i = 0; i < 3; i++) {
    int n = strlen(&arg[4 + i][2]) + 1;
    var[i] = new char[n];
    strcpy(var[i], &arg[4 + i][2]);
  }
  // read last input param
  seed = force->inumeric(FLERR, arg[7]);

  // read optional param
  demflag = 0;

  //dinika - set diff mask to -1
  diff_mask = -1;

  int iarg = 8;
  while (iarg < narg) {
    if (strcmp(arg[iarg], "demflag") == 0) {
      demflag = force->inumeric(FLERR, arg[iarg + 1]);
      if (demflag != 0 && demflag != 1)
        error->all(FLERR, "Illegal fix divide command: demflag");
      iarg += 2;
    } else
      error->all(FLERR, "Illegal fix divide command");
  }

  if (seed <= 0)
    error->all(FLERR, "Illegal fix divide command: seed is negative");

  // Random number generator, same for all procs
  random = new RanPark(lmp, seed);
  // get computational domain size
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
  // instance of nufeb biology
  bio = avec->bio;
  // force reneighbor list
  force_reneighbor = 1;
  next_reneighbor = update->ntimestep + 1;
}

/* ---------------------------------------------------------------------- */

FixPDivideTa::~FixPDivideTa() {
  delete random;

  int i;
  for (i = 0; i < 3; i++) {
    delete[] var[i];
  }
  delete[] var;
  delete[] ivar;
}

/* ---------------------------------------------------------------------- */

int FixPDivideTa::setmask() {
  int mask = 0;
  mask |= POST_INTEGRATE;
  return mask;
}

/* ----------------------------------------------------------------------
 if need to restore per-atom quantities, create new fix STORE styles
 ------------------------------------------------------------------------- */

void FixPDivideTa::init() {
  if (!atom->radius_flag)
    error->all(FLERR, "Fix divide requires atom attribute diameter");

  for (int n = 0; n < 3; n++) {
    ivar[n] = input->variable->find(var[n]);
    if (ivar[n] < 0)
      error->all(FLERR, "Variable name for fix divide does not exist");
    if (!input->variable->equalstyle(ivar[n]))
      error->all(FLERR, "Variable for fix divide is invalid style");
  }

  div_dia = input->variable->compute_equal(ivar[0]);
  asym = input->variable->compute_equal(ivar[1]);
  max_division_counter = input->variable->compute_equal(ivar[2]);

  //dinika's edits - adding division counter
  int nlocal = atom->nlocal;
  for (int i = 0; i < nlocal; i++) {
	if (atom->mask[i] & groupbit) {
	   avec->d_counter[i] = bio->division_counter[atom->type[i]];
    }
  }
  //modify atom mask
  for (int i = 1; i < group->ngroup; i++) {
    if (strcmp(group->names[i],"DIFF") == 0) {
      diff_mask = pow(2, i) + 1;
      break;
    }
  }
  if (diff_mask < 0) error->all(FLERR, "Cannot get DIFF group.");
}

void FixPDivideTa::post_integrate() {

  if (nevery == 0)
    return;
  if (update->ntimestep % nevery)
    return;
  if (demflag)
    return;

  int nlocal = atom->nlocal;

  for (int i = 0; i < nlocal; i++) {
    //this groupbit will allow the input script to set each cell type to divide
    // i.e. if set fix d1 STEM 50 .. , fix d1 TA ... etc
    if (atom->mask[i] & groupbit) {
      double density = atom->rmass[i] / (4.0 * MY_PI / 3.0 * atom->radius[i] * atom->radius[i] * atom->radius[i]);
      double newX, newY, newZ;

      // get type
      type_id = atom->type[i];
      type_name = bio->tname[type_id];
      //set differentiated cell type
      int diff_id = bio->find_typeid("diff");
      int ta_id = bio->find_typeid("ta");

      int parentDivisionCount = avec->d_counter[i];
      //printf("parent division counter is %i\n", parentDivisionCount);
      int childDivisionCount = 0;

      //random generator to set probabilities of division
      std::random_device rd;  //Will be used to obtain a seed for the random number engine
      std::mt19937 gen(rd()); //Standard mersenne_twister_engine seeded with rd()
      std::uniform_real_distribution<double>  distribution(0.0, 1.0);
      double rand = 0.0;
      for (int i = 0; i < nlocal; i++){
    	  rand = distribution(gen);
      }

      if (atom->rmass[i] * 2 >= div_dia ){
    	  if (parentDivisionCount >= max_division_counter){ //if TA cell division counter has reached the max, only divide to diff cells
    		  parentType = diff_id;
    		  childType = diff_id;
    		  parentMask = diff_mask;
    		  childMask = diff_mask;
    		  parentDivisionCount = avec->d_counter[i] + 1;
    		  childDivisionCount = 0;
    	  } else if (parentDivisionCount < max_division_counter && rand < asym){ //asymmetric division
    		  parentType = type_id;
    		  childType = diff_id;
    		  parentMask = atom->mask[i];
    		  childMask = diff_mask;
    		  parentDivisionCount = avec->d_counter[i] + 1;
    		  childDivisionCount = 0;
    	  } else { //self proliferate
    		  parentType = type_id;
			  childType = type_id;
			  parentMask = atom->mask[i];
			  childMask = atom->mask[i];
			  parentDivisionCount = avec->d_counter[i] + 1;
			  childDivisionCount = 0;
    	  }

		double parentMass = atom->rmass[i];
		double childMass = atom->rmass[i];

        //outer mass for parent and child
        double parentOuterMass = avec->outer_mass[i];
        double childOuterMass = parentOuterMass;

        // forces are the same for both parent and child, x, y and z axis
        double parentfx = atom->f[i][0];
        double childfx = atom->f[i][0];

        double parentfy = atom->f[i][1];
        double childfy = atom->f[i][1];

        double parentfz = atom->f[i][2];
        double childfz = atom->f[i][2];

        double thetaD = random->uniform() * 2 * MY_PI;
        double phiD = random->uniform() * (MY_PI);

        double oldX = atom->x[i][0];
        double oldY = atom->x[i][1];
        double oldZ = atom->x[i][2];

        //Update parent
        atom->rmass[i] = parentMass;
        atom->f[i][0] = parentfx;
        atom->f[i][1] = parentfy;
        atom->f[i][2] = parentfz;
        //todo check what was declared as a parent radius
        atom->radius[i] = pow(((6 * atom->rmass[i]) / (density * MY_PI)), (1.0 / 3.0)) * 0.5;
        avec->outer_radius[i] = atom->radius[i];
        newX = oldX + (avec->outer_radius[i] * cos(thetaD) * sin(phiD) * DELTA);
        newY = oldY + (avec->outer_radius[i] * sin(thetaD) * sin(phiD) * DELTA);
        newZ = oldZ + (avec->outer_radius[i] * cos(phiD) * DELTA);
        if (newX - avec->outer_radius[i] < xlo) {
          newX = xlo + avec->outer_radius[i];
        } else if (newX + avec->outer_radius[i] > xhi) {
          newX = xhi - avec->outer_radius[i];
        }
        if (newY - avec->outer_radius[i] < ylo) {
          newY = ylo + avec->outer_radius[i];
        } else if (newY + avec->outer_radius[i] > yhi) {
          newY = yhi - avec->outer_radius[i];
        }
        if (newZ - avec->outer_radius[i] < zlo) {
          newZ = zlo + avec->outer_radius[i];
        } else if (newZ + avec->outer_radius[i] > zhi) {
          newZ = zhi - avec->outer_radius[i];
        }
        atom->x[i][0] = newX;
        atom->x[i][1] = newY;
        atom->x[i][2] = newZ;
        atom->type[i] = parentType;
        atom->mask[i] = parentMask;
        avec->d_counter[i] = parentDivisionCount;

        //create child
        double childRadius = pow(((6 * childMass) / (density * MY_PI)), (1.0 / 3.0)) * 0.5;
        double childOuterRadius = childRadius;
        double* coord = new double[3];
        newX = oldX - (childOuterRadius * cos(thetaD) * sin(phiD) * DELTA);
        newY = oldY - (childOuterRadius * sin(thetaD) * sin(phiD) * DELTA);
        newZ = oldZ - (childOuterRadius * cos(phiD) * DELTA);
        if (newX - childOuterRadius < xlo) {
          newX = xlo + childOuterRadius;
        } else if (newX + childOuterRadius > xhi) {
          newX = xhi - childOuterRadius;
        }
        if (newY - childOuterRadius < ylo) {
          newY = ylo + childOuterRadius;
        } else if (newY + childOuterRadius > yhi) {
          newY = yhi - childOuterRadius;
        }
        if (newZ - childOuterRadius < zlo) {
          newZ = zlo + childOuterRadius;
        } else if (newZ + childOuterRadius > zhi) {
          newZ = zhi - childOuterRadius;
        }
        //coordinates should be the same as parent
        coord[0] = newX;
        coord[1] = newY;
        coord[2] = newZ;
        // create new atom
        int n = 0;
        atom->avec->create_atom(atom->type[i], coord);
        n = atom->nlocal - 1;

        atom->tag[n] = 0;
        atom->image[n] = atom->image[i];

        atom->v[n][0] = atom->v[i][0];
        atom->v[n][1] = atom->v[i][1];
        atom->v[n][2] = atom->v[i][2];
        atom->f[n][0] = atom->f[i][0];
        atom->f[n][1] = atom->f[i][1];
        atom->f[n][2] = atom->f[i][2];

        atom->rmass[n] = childMass;
        avec->outer_mass[n] = childOuterMass;

        atom->f[n][0] = childfx;
        atom->f[n][1] = childfy;
        atom->f[n][2] = childfz;

        atom->radius[n] = childRadius;
        avec->outer_radius[n] = childOuterRadius;

        atom->type[n] = childType;
        atom->mask[n] = childMask;
        avec->d_counter[n] = childDivisionCount;

        modify->create_attribute(n);

        delete[] coord;
      }
    }
  }

  bigint nblocal = atom->nlocal;
  MPI_Allreduce(&nblocal, &atom->natoms, 1, MPI_LMP_BIGINT, MPI_SUM, world);
  if (atom->natoms < 0 || atom->natoms >= MAXBIGINT)
    error->all(FLERR, "Too many total atoms");

  if (atom->tag_enable)
    atom->tag_extend();
  atom->tag_check();

  if (atom->map_style) {
    atom->nghost = 0;
    atom->map_init();
    atom->map_set();
  }

  // trigger immediate reneighboring
  next_reneighbor = update->ntimestep;
}

/* ---------------------------------------------------------------------- */

int FixPDivideTa::modify_param(int narg, char **arg) {
  if (strcmp(arg[0], "demflag") == 0) {
    if (narg != 2)
      error->all(FLERR, "Illegal fix_modify command");
    demflag = force->inumeric(FLERR, arg[1]);
    if (demflag != 1 && demflag != 0)
      error->all(FLERR, "Illegal fix_modify command: demflag");
    return 2;
  }
  return 0;
}
