/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "create_pso_atoms.h"
#include "atom.h"
#include "molecule.h"
#include "comm.h"
#include "irregular.h"
#include "modify.h"
#include "force.h"
#include "special.h"
#include "fix.h"
#include "compute.h"
#include "domain.h"
#include "lattice.h"
#include "region.h"
#include "input.h"
#include "variable.h"
#include "random_park.h"
#include "random_mars.h"
#include "math_extra.h"
#include "math_const.h"
#include "error.h"
#include "bio.h"
#include "fix_bio_kinetics.h"
#include "compute_bio_height.h"
#include "fix_bio_kinetics_diffusion.h"
#include "fix_bio_kinetics_monod.h"
#include "group.h"
#include "atom_vec_bio.h"

#include <vector>
#include <algorithm>
#include <iterator>
#include <random>
#include <iostream>

using namespace LAMMPS_NS;
using namespace MathConst;

#define BIG 1.0e30
#define EPSILON 1.0e-6

enum{BOX,REGION,SINGLE,RANDOM,STEM}; //DINIKA - add stem
enum{ATOM,MOLECULE};
enum{LAYOUT_UNIFORM,LAYOUT_NONUNIFORM,LAYOUT_TILED};    // several files

/* ---------------------------------------------------------------------- */

CreatePsoAtoms::CreatePsoAtoms(LAMMPS *lmp) : Pointers(lmp) {}

/* ---------------------------------------------------------------------- */

void CreatePsoAtoms::command(int narg, char **arg)
{
  if (domain->box_exist == 0)
    error->all(FLERR,"Create_atoms command before simulation box is defined");
  if (modify->nfix_restart_peratom)
    error->all(FLERR,"Cannot create_atoms after "
               "reading restart file with per-atom info");

  // parse arguments

  if (narg < 2) error->all(FLERR,"Illegal create_pso_atoms command");
  ntype = force->inumeric(FLERR,arg[0]);
  //printf("cell type is %d\n", ntype);

  int iarg;
  if (strcmp(arg[1],"box") == 0) {
    style = BOX;
    iarg = 2;
  } else if (strcmp(arg[1],"region") == 0) {
    style = REGION;
    if (narg < 3) error->all(FLERR,"Illegal create_pso_atoms command");
    nregion = domain->find_region(arg[2]);
    if (nregion == -1) error->all(FLERR,
                                  "Create_pso_atoms region ID does not exist");
    domain->regions[nregion]->init();
    domain->regions[nregion]->prematch();
    iarg = 3;;
  } else if (strcmp(arg[1],"single") == 0) {
    style = SINGLE;
    if (narg < 5) error->all(FLERR,"Illegal create_pso_atoms command");
    xone[0] = force->numeric(FLERR,arg[2]);
    xone[1] = force->numeric(FLERR,arg[3]);
    xone[2] = force->numeric(FLERR,arg[4]);
    iarg = 5;
  } else if (strcmp(arg[1],"random") == 0) {
    style = RANDOM;
    if (narg < 5) error->all(FLERR,"Illegal create_pso_atoms command");
    nrandom = force->inumeric(FLERR,arg[2]);
    seed = force->inumeric(FLERR,arg[3]);
    if (strcmp(arg[4],"NULL") == 0) nregion = -1;
    else {
      nregion = domain->find_region(arg[4]);
      if (nregion == -1) error->all(FLERR,
                                    "Create_atoms region ID does not exist");
      domain->regions[nregion]->init();
      domain->regions[nregion]->prematch();
    }
    iarg = 5;
  } else if (strcmp(arg[1],"stem") == 0) {     //DINIKA - make it like in create stem, cutoff etc
    style = STEM;
    if (narg < 7) error->all(FLERR,"Illegal create_pso_atoms command");
    cutoff = force->numeric(FLERR, arg[2]);
    //printf("the cut off number is %e \n", cutoff);
    density = force->numeric(FLERR, arg[3]);
    //printf("the density is %f \n", density);
    diameter = force->numeric(FLERR, arg[4]);
    //printf("the diameter is %f \n", diameter);
    //get the number of sc to initialise
    num_sc = force->inumeric(FLERR, arg[5]);
    //printf("the num sc is %d \n", num_sc);
    seed = force->inumeric(FLERR, arg[6]);
    //printf("the seed is %d \n", seed);
    iarg = 7;
  } else error->all(FLERR,"Illegal create_pso_atoms command");

  // process optional keywords

  int scaleflag = 1;
  remapflag = 0;
  mode = ATOM;
  int molseed;
  varflag = 0;
  vstr = xstr = ystr = zstr = NULL;
  quatone[0] = quatone[1] = quatone[2] = 0.0;

  nbasis = domain->lattice->nbasis;
  basistype = new int[nbasis];
  for (int i = 0; i < nbasis; i++) basistype[i] = ntype;

  //DINIKA - include avec
  avec_bio = (AtomVecBio *) atom->style_match("bio");

  while (iarg < narg) {
    if (strcmp(arg[iarg],"basis") == 0) {
      if (iarg+3 > narg) error->all(FLERR,"Illegal create_pso_atoms command");
      int ibasis = force->inumeric(FLERR,arg[iarg+1]);
      int itype = force->inumeric(FLERR,arg[iarg+2]);
      if (ibasis <= 0 || ibasis > nbasis || itype <= 0 || itype > atom->ntypes)
        error->all(FLERR,"Invalid basis setting in create_pso_atoms command");
      basistype[ibasis-1] = itype;
      iarg += 3;
    } else if (strcmp(arg[iarg],"remap") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal create_pso_atoms command");
      if (strcmp(arg[iarg+1],"yes") == 0) remapflag = 1;
      else if (strcmp(arg[iarg+1],"no") == 0) remapflag = 0;
      else error->all(FLERR,"Illegal create_pso_atoms command");
      iarg += 2;
    } else if (strcmp(arg[iarg],"mol") == 0) {
      if (iarg+3 > narg) error->all(FLERR,"Illegal create_pso_atoms command");
      int imol = atom->find_molecule(arg[iarg+1]);
      if (imol == -1) error->all(FLERR,"Molecule template ID for "
                                 "create_atoms does not exist");
      if (atom->molecules[imol]->nset > 1 && comm->me == 0)
        error->warning(FLERR,"Molecule template for "
                       "create_atoms has multiple molecules");
      mode = MOLECULE;
      onemol = atom->molecules[imol];
      molseed = force->inumeric(FLERR,arg[iarg+2]);
      iarg += 3;
    } else if (strcmp(arg[iarg],"units") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal create_pso_atoms command");
      if (strcmp(arg[iarg+1],"box") == 0) scaleflag = 0;
      else if (strcmp(arg[iarg+1],"lattice") == 0) scaleflag = 1;
      else error->all(FLERR,"Illegal create_pso_atoms command");
      iarg += 2;
    } else if (strcmp(arg[iarg],"var") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal create_pso_atoms command");
      delete [] vstr;
      int n = strlen(arg[iarg+1]) + 1;
      vstr = new char[n];
      strcpy(vstr,arg[iarg+1]);
      varflag = 1;
      iarg += 2;
    } else if (strcmp(arg[iarg],"set") == 0) {
      if (iarg+3 > narg) error->all(FLERR,"Illegal create_pso_atoms command");
      if (strcmp(arg[iarg+1],"x") == 0) {
        delete [] xstr;
        int n = strlen(arg[iarg+2]) + 1;
        xstr = new char[n];
        strcpy(xstr,arg[iarg+2]);
      } else if (strcmp(arg[iarg+1],"y") == 0) {
        delete [] ystr;
        int n = strlen(arg[iarg+2]) + 1;
        ystr = new char[n];
        strcpy(ystr,arg[iarg+2]);
      } else if (strcmp(arg[iarg+1],"z") == 0) {
        delete [] zstr;
        int n = strlen(arg[iarg+2]) + 1;
        zstr = new char[n];
        strcpy(zstr,arg[iarg+2]);
      } else error->all(FLERR,"Illegal create_pso_atoms command");
      iarg += 3;
    } else if (strcmp(arg[iarg],"rotate") == 0) {
      if (style != SINGLE)
        error->all(FLERR,"Cannot use create_pso_atoms rotate unless single style");
      if (iarg+5 > narg) error->all(FLERR,"Illegal create_pso_atoms command");
      double thetaone;
      double axisone[3];
      thetaone = force->numeric(FLERR,arg[iarg+1]);
      axisone[0] = force->numeric(FLERR,arg[iarg+2]);
      axisone[1] = force->numeric(FLERR,arg[iarg+3]);
      axisone[2] = force->numeric(FLERR,arg[iarg+4]);
      if (axisone[0] == 0.0 && axisone[1] == 0.0 && axisone[2] == 0.0)
        error->all(FLERR,"Illegal create_pso_atoms command");
      if (domain->dimension == 2 && (axisone[0] != 0.0 || axisone[1] != 0.0))
        error->all(FLERR,"Invalid create_pso_atoms rotation vector for 2d model");
      MathExtra::norm3(axisone);
      MathExtra::axisangle_to_quat(axisone,thetaone,quatone);
      iarg += 5;
    } else error->all(FLERR,"Illegal create_pso_atoms command");
  }

  // error checks

  if (mode == ATOM && (ntype <= 0 || ntype > atom->ntypes))
    error->all(FLERR,"Invalid atom type in create_pso_atoms command");

  if (style == RANDOM) {
    if (nrandom < 0) error->all(FLERR,"Illegal create_pso_atoms command");
    if (seed <= 0) error->all(FLERR,"Illegal create_pso_atoms command");
  }
 //DINIKA - new error checks
  if (style == STEM) {
	  if (cutoff < 0) error->all(FLERR,"Illegal create_pso_atoms command");
	  if (seed <= 0) error->all(FLERR, "Illegal create stem command: seed is negative");
	  if (num_sc <= 0) error->all(FLERR, "Number of stem cells to initialise must be more than 1");
	  //if (max_surface <= 0) error->all(FLERR, "Max number of surfaces cannot be less than or equal to 0");
  }
  // error check and further setup for mode = MOLECULE

  ranmol = NULL;
  if (mode == MOLECULE) {
    if (onemol->xflag == 0)
      error->all(FLERR,"create_pso_atoms molecule must have coordinates");
    if (onemol->typeflag == 0)
      error->all(FLERR,"create_pso_atoms molecule must have atom types");
    if (ntype+onemol->ntypes <= 0 || ntype+onemol->ntypes > atom->ntypes)
      error->all(FLERR,"Invalid atom type in create_pso_atoms mol command");
    if (onemol->tag_require && !atom->tag_enable)
      error->all(FLERR,
                 "create_pso_atoms molecule has atom IDs, but system does not");
    onemol->check_attributes(0);

    // create_atoms uses geoemetric center of molecule for insertion

    onemol->compute_center();

    // molecule random number generator, different for each proc

    ranmol = new RanMars(lmp,molseed+comm->me);
  }

  // error check and further setup for variable test

  if (!vstr && (xstr || ystr || zstr))
    error->all(FLERR,"Incomplete use of variables in create_atoms command");
  if (vstr && (!xstr && !ystr && !zstr))
    error->all(FLERR,"Incomplete use of variables in create_atoms command");

  if (varflag) {
    vvar = input->variable->find(vstr);
    if (vvar < 0)
      error->all(FLERR,"Variable name for create_atoms does not exist");
    if (!input->variable->equalstyle(vvar))
      error->all(FLERR,"Variable for create_atoms is invalid style");

    if (xstr) {
      xvar = input->variable->find(xstr);
      if (xvar < 0)
        error->all(FLERR,"Variable name for create_atoms does not exist");
      if (!input->variable->internalstyle(xvar))
        error->all(FLERR,"Variable for create_atoms is invalid style");
    }
    if (ystr) {
      yvar = input->variable->find(ystr);
      if (yvar < 0)
        error->all(FLERR,"Variable name for create_atoms does not exist");
      if (!input->variable->internalstyle(yvar))
        error->all(FLERR,"Variable for create_atoms is invalid style");
    }
    if (zstr) {
      zvar = input->variable->find(zstr);
      if (zvar < 0)
        error->all(FLERR,"Variable name for create_atoms does not exist");
      if (!input->variable->internalstyle(zvar))
        error->all(FLERR,"Variable for create_atoms is invalid style");
    }
  }

  // demand non-none lattice be defined for BOX and REGION
  // else setup scaling for SINGLE and RANDOM
  // could use domain->lattice->lattice2box() to do conversion of
  //   lattice to box, but not consistent with other uses of units=lattice
  // triclinic remapping occurs in add_single()

  if (style == BOX || style == REGION) {
    if (nbasis == 0)
      error->all(FLERR,"Cannot create atoms with undefined lattice");
  } else if (scaleflag == 1) {
    xone[0] *= domain->lattice->xlattice;
    xone[1] *= domain->lattice->ylattice;
    xone[2] *= domain->lattice->zlattice;
  }

  // set bounds for my proc in sublo[3] & subhi[3]
  // if periodic and style = BOX or REGION, i.e. using lattice:
  //   should create exactly 1 atom when 2 images are both "on" the boundary
  //   either image may be slightly inside/outside true box due to round-off
  //   if I am lo proc, decrement lower bound by EPSILON
  //     this will insure lo image is created
  //   if I am hi proc, decrement upper bound by 2.0*EPSILON
  //     this will insure hi image is not created
  //   thus insertion box is EPSILON smaller than true box
  //     and is shifted away from true boundary
  //     which is where atoms are likely to be generated

  triclinic = domain->triclinic;

  double epsilon[3];
  if (triclinic) epsilon[0] = epsilon[1] = epsilon[2] = EPSILON;
  else {
    epsilon[0] = domain->prd[0] * EPSILON;
    epsilon[1] = domain->prd[1] * EPSILON;
    epsilon[2] = domain->prd[2] * EPSILON;
  }

  if (triclinic == 0) {
    sublo[0] = domain->sublo[0]; subhi[0] = domain->subhi[0];
    sublo[1] = domain->sublo[1]; subhi[1] = domain->subhi[1];
    sublo[2] = domain->sublo[2]; subhi[2] = domain->subhi[2];
  } else {
    sublo[0] = domain->sublo_lamda[0]; subhi[0] = domain->subhi_lamda[0];
    sublo[1] = domain->sublo_lamda[1]; subhi[1] = domain->subhi_lamda[1];
    sublo[2] = domain->sublo_lamda[2]; subhi[2] = domain->subhi_lamda[2];
  }

  if (style == BOX || style == REGION) {
    if (comm->layout != LAYOUT_TILED) {
      if (domain->xperiodic) {
        if (comm->myloc[0] == 0) sublo[0] -= epsilon[0];
        if (comm->myloc[0] == comm->procgrid[0]-1) subhi[0] -= 2.0*epsilon[0];
      }
      if (domain->yperiodic) {
        if (comm->myloc[1] == 0) sublo[1] -= epsilon[1];
        if (comm->myloc[1] == comm->procgrid[1]-1) subhi[1] -= 2.0*epsilon[1];
      }
      if (domain->zperiodic) {
        if (comm->myloc[2] == 0) sublo[2] -= epsilon[2];
        if (comm->myloc[2] == comm->procgrid[2]-1) subhi[2] -= 2.0*epsilon[2];
      }
    } else {
      if (domain->xperiodic) {
        if (comm->mysplit[0][0] == 0.0) sublo[0] -= epsilon[0];
        if (comm->mysplit[0][1] == 1.0) subhi[0] -= 2.0*epsilon[0];
      }
      if (domain->yperiodic) {
        if (comm->mysplit[1][0] == 0.0) sublo[1] -= epsilon[1];
        if (comm->mysplit[1][1] == 1.0) subhi[1] -= 2.0*epsilon[1];
      }
      if (domain->zperiodic) {
        if (comm->mysplit[2][0] == 0.0) sublo[2] -= epsilon[2];
        if (comm->mysplit[2][1] == 1.0) subhi[2] -= 2.0*epsilon[2];
      }
    }
  }

  // clear ghost count and any ghost bonus data internal to AtomVec
  // same logic as beginning of Comm::exchange()
  // do it now b/c creating atoms will overwrite ghost atoms

  atom->nghost = 0;
  atom->avec->clear_bonus();

  // add atoms/molecules in one of 3 ways

  bigint natoms_previous = atom->natoms;
  int nlocal_previous = atom->nlocal;

  if (style == SINGLE) add_single();
  else if (style == RANDOM) add_random();
  else if (style == STEM) add_stem(); //DINIKA - add stem
  else add_lattice();

  // init per-atom fix/compute/variable values for created atoms
  
  atom->data_fix_compute_variable(nlocal_previous,atom->nlocal);

  // set new total # of atoms and error check

  bigint nblocal = atom->nlocal;
  MPI_Allreduce(&nblocal,&atom->natoms,1,MPI_LMP_BIGINT,MPI_SUM,world);
  if (atom->natoms < 0 || atom->natoms >= MAXBIGINT)
    error->all(FLERR,"Too many total atoms");

  // add IDs for newly created atoms
  // check that atom IDs are valid

  if (atom->tag_enable) atom->tag_extend();
  atom->tag_check();

  // if global map exists, reset it
  // invoke map_init() b/c atom count has grown

  if (atom->map_style) {
    atom->map_init();
    atom->map_set();
  }

  // for MOLECULE mode:
  // molecule can mean just a mol ID or bonds/angles/etc or mol templates
  // set molecule IDs for created atoms if atom->molecule_flag is set
  // reset new molecule bond,angle,etc and special values if defined
  // send atoms to new owning procs via irregular comm
  //   since not all atoms I created will be within my sub-domain
  // perform special list build if needed

  if (mode == MOLECULE) {

    int molecule_flag = atom->molecule_flag;
    int molecular = atom->molecular;
    tagint *molecule = atom->molecule;

    // molcreate = # of molecules I created

    int molcreate = (atom->nlocal - nlocal_previous) / onemol->natoms;

    // increment total bonds,angles,etc

    bigint nmolme = molcreate;
    bigint nmoltotal;
    MPI_Allreduce(&nmolme,&nmoltotal,1,MPI_LMP_BIGINT,MPI_SUM,world);
    atom->nbonds += nmoltotal * onemol->nbonds;
    atom->nangles += nmoltotal * onemol->nangles;
    atom->ndihedrals += nmoltotal * onemol->ndihedrals;
    atom->nimpropers += nmoltotal * onemol->nimpropers;

    // if atom style template
    // maxmol = max molecule ID across all procs, for previous atoms
    // moloffset = max molecule ID for all molecules owned by previous procs
    //             including molecules existing before this creation

    tagint moloffset;
    if (molecule_flag) {
      tagint max = 0;
      for (int i = 0; i < nlocal_previous; i++) max = MAX(max,molecule[i]);
      tagint maxmol;
      MPI_Allreduce(&max,&maxmol,1,MPI_LMP_TAGINT,MPI_MAX,world);
      MPI_Scan(&molcreate,&moloffset,1,MPI_INT,MPI_SUM,world);
      moloffset = moloffset - molcreate + maxmol;
    }

    // loop over molecules I created
    // set their molecule ID
    // reset their bond,angle,etc and special values

    int natoms = onemol->natoms;
    tagint offset = 0;

    tagint *tag = atom->tag;
    int *num_bond = atom->num_bond;
    int *num_angle = atom->num_angle;
    int *num_dihedral = atom->num_dihedral;
    int *num_improper = atom->num_improper;
    tagint **bond_atom = atom->bond_atom;
    tagint **angle_atom1 = atom->angle_atom1;
    tagint **angle_atom2 = atom->angle_atom2;
    tagint **angle_atom3 = atom->angle_atom3;
    tagint **dihedral_atom1 = atom->dihedral_atom1;
    tagint **dihedral_atom2 = atom->dihedral_atom2;
    tagint **dihedral_atom3 = atom->dihedral_atom3;
    tagint **dihedral_atom4 = atom->dihedral_atom4;
    tagint **improper_atom1 = atom->improper_atom1;
    tagint **improper_atom2 = atom->improper_atom2;
    tagint **improper_atom3 = atom->improper_atom3;
    tagint **improper_atom4 = atom->improper_atom4;
    int **nspecial = atom->nspecial;
    tagint **special = atom->special;

    int ilocal = nlocal_previous;
    for (int i = 0; i < molcreate; i++) {
      if (tag) offset = tag[ilocal]-1;
      for (int m = 0; m < natoms; m++) {
        if (molecule_flag) molecule[ilocal] = moloffset + i+1;
        if (molecular == 2) {
          atom->molindex[ilocal] = 0;
          atom->molatom[ilocal] = m;
        } else if (molecular) {
          if (onemol->bondflag)
            for (int j = 0; j < num_bond[ilocal]; j++)
              bond_atom[ilocal][j] += offset;
          if (onemol->angleflag)
            for (int j = 0; j < num_angle[ilocal]; j++) {
              angle_atom1[ilocal][j] += offset;
              angle_atom2[ilocal][j] += offset;
              angle_atom3[ilocal][j] += offset;
            }
          if (onemol->dihedralflag)
            for (int j = 0; j < num_dihedral[ilocal]; j++) {
              dihedral_atom1[ilocal][j] += offset;
              dihedral_atom2[ilocal][j] += offset;
              dihedral_atom3[ilocal][j] += offset;
              dihedral_atom4[ilocal][j] += offset;
            }
          if (onemol->improperflag)
            for (int j = 0; j < num_improper[ilocal]; j++) {
              improper_atom1[ilocal][j] += offset;
              improper_atom2[ilocal][j] += offset;
              improper_atom3[ilocal][j] += offset;
              improper_atom4[ilocal][j] += offset;
            }
          if (onemol->specialflag)
            for (int j = 0; j < nspecial[ilocal][2]; j++)
              special[ilocal][j] += offset;
        }
        ilocal++;
      }
    }

    // perform irregular comm to migrate atoms to new owning procs

    double **x = atom->x;
    imageint *image = atom->image;
    int nlocal = atom->nlocal;
    for (int i = 0; i < nlocal; i++) domain->remap(x[i],image[i]);

    if (domain->triclinic) domain->x2lamda(atom->nlocal);
    domain->reset_box();
    Irregular *irregular = new Irregular(lmp);
    irregular->migrate_atoms(1);
    delete irregular;
    if (domain->triclinic) domain->lamda2x(atom->nlocal);
  }

  // clean up

  delete ranmol;
  if (domain->lattice) delete [] basistype;
  delete [] vstr;
  delete [] xstr;
  delete [] ystr;
  delete [] zstr;

  // print status

  if (comm->me == 0) {
    if (screen)
      fprintf(screen,"Created " BIGINT_FORMAT " atoms\n",
              atom->natoms-natoms_previous);
    if (logfile)
      fprintf(logfile,"Created " BIGINT_FORMAT " atoms\n",
              atom->natoms-natoms_previous);
  }

  // for MOLECULE mode:
  // create special bond lists for molecular systems,
  //   but not for atom style template
  // only if onemol added bonds but not special info

  if (mode == MOLECULE) {
    if (atom->molecular == 1 && onemol->bondflag && !onemol->specialflag) {
      Special special(lmp);
      special.build();
    }
  }
}

/* ----------------------------------------------------------------------
   add single atom with coords at xone if it's in my sub-box
   if triclinic, xone is in lamda coords
------------------------------------------------------------------------- */

void CreatePsoAtoms::add_single()
{
  // remap atom if requested

  if (remapflag) {
    imageint imagetmp = ((imageint) IMGMAX << IMG2BITS) |
      ((imageint) IMGMAX << IMGBITS) | IMGMAX;
    domain->remap(xone,imagetmp);
  }

  // if triclinic, convert to lamda coords (0-1)

  double lamda[3],*coord;
  if (triclinic) {
    domain->x2lamda(xone,lamda);
    coord = lamda;
  } else coord = xone;

  // if atom/molecule is in my subbox, create it

  if (coord[0] >= sublo[0] && coord[0] < subhi[0] &&
      coord[1] >= sublo[1] && coord[1] < subhi[1] &&
      coord[2] >= sublo[2] && coord[2] < subhi[2]) {
    if (mode == ATOM) atom->avec->create_atom(ntype,xone);
    else if (quatone[0] == 0.0 && quatone[1] == 0.0 && quatone[2] == 0.0)
      add_molecule(xone);
    else add_molecule(xone,quatone);
  }
}

/* ----------------------------------------------------------------------
   add Nrandom atoms at random locations
------------------------------------------------------------------------- */

void CreatePsoAtoms::add_random()
{
  double xlo,ylo,zlo,xhi,yhi,zhi,zmid;
  double lamda[3],*coord;
  double *boxlo,*boxhi;

  // random number generator, same for all procs

  RanPark *random = new RanPark(lmp,seed);

  // bounding box for atom creation
  // in real units, even if triclinic
  // only limit bbox by region if its bboxflag is set (interior region)

  if (triclinic == 0) {
    xlo = domain->boxlo[0]; xhi = domain->boxhi[0];
    ylo = domain->boxlo[1]; yhi = domain->boxhi[1];
    zlo = domain->boxlo[2]; zhi = domain->boxhi[2];
    zmid = zlo + 0.5*(zhi-zlo);
  } else {
    xlo = domain->boxlo_bound[0]; xhi = domain->boxhi_bound[0];
    ylo = domain->boxlo_bound[1]; yhi = domain->boxhi_bound[1];
    zlo = domain->boxlo_bound[2]; zhi = domain->boxhi_bound[2];
    zmid = zlo + 0.5*(zhi-zlo);
    boxlo = domain->boxlo_lamda;
    boxhi = domain->boxhi_lamda;
  }

  if (nregion >= 0 && domain->regions[nregion]->bboxflag) {
    xlo = MAX(xlo,domain->regions[nregion]->extent_xlo);
    xhi = MIN(xhi,domain->regions[nregion]->extent_xhi);
    ylo = MAX(ylo,domain->regions[nregion]->extent_ylo);
    yhi = MIN(yhi,domain->regions[nregion]->extent_yhi);
    zlo = MAX(zlo,domain->regions[nregion]->extent_zlo);
    zhi = MIN(zhi,domain->regions[nregion]->extent_zhi);
  }

  // generate random positions for each new atom/molecule within bounding box
  // iterate until atom is within region, variable, and triclinic simulation box
  // if final atom position is in my subbox, create it

  if (xlo > xhi || ylo > yhi || zlo > zhi)
    error->all(FLERR,"No overlap of box and region for create_atoms");

  int valid;
  for (int i = 0; i < nrandom; i++) {
    while (1) {
      xone[0] = xlo + random->uniform() * (xhi-xlo);
      xone[1] = ylo + random->uniform() * (yhi-ylo);
      xone[2] = zlo + random->uniform() * (zhi-zlo);
      if (domain->dimension == 2) xone[2] = zmid;

      valid = 1;
      if (nregion >= 0 &&
          domain->regions[nregion]->match(xone[0],xone[1],xone[2]) == 0)
        valid = 0;
      if (varflag && vartest(xone) == 0) valid = 0;
      if (triclinic) {
        domain->x2lamda(xone,lamda);
        coord = lamda;
        if (coord[0] < boxlo[0] || coord[0] >= boxhi[0] ||
            coord[1] < boxlo[1] || coord[1] >= boxhi[1] ||
            coord[2] < boxlo[2] || coord[2] >= boxhi[2]) valid = 0;
      } else coord = xone;

      if (valid) break;
    }

    // if triclinic, coord is now in lamda units

    if (coord[0] >= sublo[0] && coord[0] < subhi[0] &&
        coord[1] >= sublo[1] && coord[1] < subhi[1] &&
        coord[2] >= sublo[2] && coord[2] < subhi[2]) {
      if (mode == ATOM) atom->avec->create_atom(ntype,xone);
      else add_molecule(xone);
    }
  }

  // clean-up

  delete random;
}

/* ----------------------------------------------------------------------
  **DINIKA**

   add stem cells on the top of bm --> should include post integrate stuffs
   in previous fix
------------------------------------------------------------------------- */

void CreatePsoAtoms::add_stem()
{
  // random number generator, same for all procs

  //RanPark *random = new RanPark(lmp,seed);

	if (num_sc > 0) {
		//refresh list and get all the empty locations
		emptyList.clear();
		empty_loc();
		int atomId;
		std::vector<int> freeLoc;
		//shuffle the vector and assign to a new vector with the number of sc to initialise
		std::random_shuffle (emptyList.begin(), emptyList.end());
		freeLoc.assign(emptyList.begin(), emptyList.begin() + (num_sc));

		 //***bowen*** get mask
    for (int i = 1; i < group->ngroup; i++) {
    if (strcmp(group->names[i],"STEM") == 0) {
      sc_mask = pow(2, i) + 1;
      break;
      }
    }

	  if (sc_mask < 0) error->all(FLERR, "Cannot find STEM group.");
	    //***bowen*** get type id
		int stem_id = avec_bio->bio->find_typeid("stem");
		double r = diameter/2;

		for (int i = 0; i < freeLoc.size(); i++){
			double* coord = new double[3];

			atomId = freeLoc[i];

			 //***bowen*** x y are same with surface atom, z is 1 diameter higher
			coord[0] = atom->x[atomId][0];
			coord[1] = atom->x[atomId][1];
			coord[2] = atom->x[atomId][2] + atom->radius[atomId] * 2;
			//printf("i=%i x=%e y=%e z=%e \n", i, coord[0],coord[1],coord[2]);
			int n = 0;
			//create new sc to initialise on surface
			avec_bio->create_atom(stem_id, coord);
			//gets the new atom id
			n = atom->nlocal - 1;

			atom->radius[n] = r;
			atom->rmass[n] = 4.0*3.1415926/3.0*r*r*r*density;
			avec_bio->outer_mass[n] = atom->rmass[n];
			avec_bio->outer_radius[n] = r;

			atom->mask[n] = sc_mask;
			atom->tag[n] = 0;

	    delete[] coord;
	  }
	}
}

//create a list of all the empty locations
void CreatePsoAtoms::empty_loc() {
	//get cutoff from inputscript
	e_cutoff = cutoff;
	nlist.clear();
	emptyList.clear();
	//build neighbor list
	neighbor_list();
	// free surface particles & bottom particles
	int max_surface = 6;
	double minx, miny, minz, maxx, maxy;
	double gminx, gminy, gminz, gmaxx, gmaxy;

	minx = miny = minz = 10;
	maxx = maxy = 0;

	for (int i = 0; i < nlist.size(); i++) {
	  if(nlist[i].size() > max_surface) error->all(FLERR, "Too many neighbors, adjust cutoff value.");
	  if(nlist[i].size() == max_surface) continue;

	  if (atom->x[i][0] < minx) minx = atom->x[i][0];
	  if (atom->x[i][1] < miny) miny = atom->x[i][1];
	  if (atom->x[i][2] < minz) minz = atom->x[i][2];
	  if (atom->x[i][0] > maxx) maxx = atom->x[i][0];
	  if (atom->x[i][1] > maxy) maxy = atom->x[i][1];
	}

	MPI_Allreduce(&minx,&gminx,1,MPI_DOUBLE,MPI_MIN,world);
	MPI_Allreduce(&miny,&gminy,1,MPI_DOUBLE,MPI_MIN,world);
	MPI_Allreduce(&minz,&gminz,1,MPI_DOUBLE,MPI_MIN,world);
	MPI_Allreduce(&maxx,&gmaxx,1,MPI_DOUBLE,MPI_MAX,world);
	MPI_Allreduce(&maxy,&gmaxy,1,MPI_DOUBLE,MPI_MAX,world);

	for (int i = 0; i < nlist.size(); i++) {
	  int surface = nlist[i].size();
	  if (surface == max_surface) continue;

	  if (atom->x[i][0] == minx) {
		surface++;
	  }
	  if (atom->x[i][1] == miny) {
		surface++;
	  }
	  if (atom->x[i][2] == minz) {
		surface++;
	  }
	  if (atom->x[i][0] == maxx) {
		surface++;
	  }
	  if (atom->x[i][1] == maxy) {
		surface++;
	  }

	  //if the atom has less than 6 surfaces, then it is a surface atom
	  if (surface < max_surface) {
		  emptyList.push_back(i);
		}
	}
}

//get a list of all the neighboring cells
void CreatePsoAtoms::neighbor_list() {

  for(int i = 0; i < atom->nlocal; i++){
    int type = atom->type[i];

    if (strcmp(avec_bio->bio->tname[type],"bm") == 0) {
      std::vector<int> subList;

      for(int j = 0; j < atom->nlocal; j++){
        int typej = atom->type[j];
        if (strcmp(avec_bio->bio->tname[typej],"bm") == 0) {
          if(i != j) {
            double xd = atom->x[i][0] - atom->x[j][0];
            double yd = atom->x[i][1] - atom->x[j][1];
            double zd = atom->x[i][2] - atom->x[j][2];

            double rsq = (xd*xd + yd*yd + zd*zd);
            double cut = (atom->radius[i] + atom->radius[j] + cutoff) * (atom->radius[i] + atom->radius[j]+ cutoff);

            if (rsq <= cut) subList.push_back(j); //push.back = adding to the list
          }
        }
      }
      nlist.push_back(subList);
     }
  }
  //printf("size of nlist is %d \n",nlist.size());
}

//function to test - prints vectors
void CreatePsoAtoms::print(std::vector<double> const &input)
{
	for (int i = 0; i < input.size(); i++) {
		std::cout << input.at(i) << ' ';
	}
}


/* ----------------------------------------------------------------------
   add many atoms by looping over lattice
------------------------------------------------------------------------- */

void CreatePsoAtoms::add_lattice()
{
  // convert 8 corners of my subdomain from box coords to lattice coords
  // for orthogonal, use corner pts of my subbox
  // for triclinic, use bounding box of my subbox
  // xyz min to max = bounding box around the domain corners in lattice space

  double bboxlo[3],bboxhi[3];

  if (triclinic == 0) {
    bboxlo[0] = domain->sublo[0]; bboxhi[0] = domain->subhi[0];
    bboxlo[1] = domain->sublo[1]; bboxhi[1] = domain->subhi[1];
    bboxlo[2] = domain->sublo[2]; bboxhi[2] = domain->subhi[2];
  } else domain->bbox(domain->sublo_lamda,domain->subhi_lamda,bboxlo,bboxhi);

  double xmin,ymin,zmin,xmax,ymax,zmax;
  xmin = ymin = zmin = BIG;
  xmax = ymax = zmax = -BIG;

  domain->lattice->bbox(1,bboxlo[0],bboxlo[1],bboxlo[2],
                        xmin,ymin,zmin,xmax,ymax,zmax);
  domain->lattice->bbox(1,bboxhi[0],bboxlo[1],bboxlo[2],
                        xmin,ymin,zmin,xmax,ymax,zmax);
  domain->lattice->bbox(1,bboxlo[0],bboxhi[1],bboxlo[2],
                        xmin,ymin,zmin,xmax,ymax,zmax);
  domain->lattice->bbox(1,bboxhi[0],bboxhi[1],bboxlo[2],
                        xmin,ymin,zmin,xmax,ymax,zmax);
  domain->lattice->bbox(1,bboxlo[0],bboxlo[1],bboxhi[2],
                        xmin,ymin,zmin,xmax,ymax,zmax);
  domain->lattice->bbox(1,bboxhi[0],bboxlo[1],bboxhi[2],
                        xmin,ymin,zmin,xmax,ymax,zmax);
  domain->lattice->bbox(1,bboxlo[0],bboxhi[1],bboxhi[2],
                        xmin,ymin,zmin,xmax,ymax,zmax);
  domain->lattice->bbox(1,bboxhi[0],bboxhi[1],bboxhi[2],
                        xmin,ymin,zmin,xmax,ymax,zmax);

  // ilo:ihi,jlo:jhi,klo:khi = loop bounds for lattice overlap of my subbox
  // overlap = any part of a unit cell (face,edge,pt) in common with my subbox
  // in lattice space, subbox is a tilted box
  // but bbox of subbox is aligned with lattice axes
  // so ilo:khi unit cells should completely tile bounding box
  // decrement lo, increment hi to avoid round-off issues in lattice->bbox(),
  //   which can lead to missing atoms in rare cases
  // extra decrement of lo if min < 0, since static_cast(-1.5) = -1

  int ilo,ihi,jlo,jhi,klo,khi;
  ilo = static_cast<int> (xmin) - 1;
  jlo = static_cast<int> (ymin) - 1;
  klo = static_cast<int> (zmin) - 1;
  ihi = static_cast<int> (xmax) + 1;
  jhi = static_cast<int> (ymax) + 1;
  khi = static_cast<int> (zmax) + 1;

  if (xmin < 0.0) ilo--;
  if (ymin < 0.0) jlo--;
  if (zmin < 0.0) klo--;

  // iterate on 3d periodic lattice of unit cells using loop bounds
  // iterate on nbasis atoms in each unit cell
  // convert lattice coords to box coords
  // add atom or molecule (on each basis point) if it meets all criteria

  double **basis = domain->lattice->basis;
  double x[3],lamda[3];
  double *coord;

  int i,j,k,m;
  for (k = klo; k <= khi; k++)
    for (j = jlo; j <= jhi; j++)
      for (i = ilo; i <= ihi; i++)
        for (m = 0; m < nbasis; m++) {

          x[0] = i + basis[m][0];
          x[1] = j + basis[m][1];
          x[2] = k + basis[m][2];

          // convert from lattice coords to box coords

          domain->lattice->lattice2box(x[0],x[1],x[2]);

          // if a region was specified, test if atom is in it

          if (style == REGION)
            if (!domain->regions[nregion]->match(x[0],x[1],x[2])) continue;

          // if variable test specified, eval variable

          if (varflag && vartest(x) == 0) continue;

          // test if atom/molecule position is in my subbox

          if (triclinic) {
            domain->x2lamda(x,lamda);
            coord = lamda;
          } else coord = x;

          if (coord[0] < sublo[0] || coord[0] >= subhi[0] ||
              coord[1] < sublo[1] || coord[1] >= subhi[1] ||
              coord[2] < sublo[2] || coord[2] >= subhi[2]) continue;

          // add the atom or entire molecule to my list of atoms

          if (mode == ATOM) atom->avec->create_atom(basistype[m],x);
          else add_molecule(x);
        }
}

/* ----------------------------------------------------------------------
   add a randomly rotated molecule with its center at center
   if quat_user set, perform requested rotation
------------------------------------------------------------------------- */

void CreatePsoAtoms::add_molecule(double *center, double *quat_user)
{
  int n;
  double r[3],rotmat[3][3],quat[4],xnew[3];

  if (quat_user) {
    quat[0] = quat_user[0]; quat[1] = quat_user[1];
    quat[2] = quat_user[2]; quat[3] = quat_user[3];
  } else {
    if (domain->dimension == 3) {
      r[0] = ranmol->uniform() - 0.5;
      r[1] = ranmol->uniform() - 0.5;
      r[2] = ranmol->uniform() - 0.5;
    } else {
      r[0] = r[1] = 0.0;
      r[2] = 1.0;
    }
    MathExtra::norm3(r);
    double theta = ranmol->uniform() * MY_2PI;
    MathExtra::axisangle_to_quat(r,theta,quat);
  }

  MathExtra::quat_to_mat(quat,rotmat);
  onemol->quat_external = quat;

  // create atoms in molecule with atom ID = 0 and mol ID = 0
  // reset in caller after all moleclues created by all procs
  // pass add_molecule_atom an offset of 0 since don't know
  //   max tag of atoms in previous molecules at this point

  int natoms = onemol->natoms;
  for (int m = 0; m < natoms; m++) {
    MathExtra::matvec(rotmat,onemol->dx[m],xnew);
    MathExtra::add3(xnew,center,xnew);
    atom->avec->create_atom(ntype+onemol->type[m],xnew);
    n = atom->nlocal - 1;
    atom->add_molecule_atom(onemol,m,n,0);
  }
}

/* ----------------------------------------------------------------------
   test a generated atom position against variable evaluation
   first set x,y,z values in internal variables
------------------------------------------------------------------------- */

int CreatePsoAtoms::vartest(double *x)
{
  if (xstr) input->variable->internal_set(xvar,x[0]);
  if (ystr) input->variable->internal_set(yvar,x[1]);
  if (zstr) input->variable->internal_set(zvar,x[2]);

  double value = input->variable->compute_equal(vvar);

  if (value == 0.0) return 0;
  return 1;
}
