#!/bin/bash
cd ${0%/*} || exit 1 # Run from this directory

echo "Installing NUFEB.."
currentDir=$PWD

#### Copy package and lib files to LAMMPS directory #####
echo "Copying packages to LAMMPS.."
cp -rf $currentDir/src/* $currentDir/lammps/src/
cp -rf $currentDir/lib/* $currentDir/lammps/lib/

echo "Configuring Makefile.lammps.."

cd $currentDir/lammps/lib/nufeb

for var in "$@"
do 
    if [ $var == "--enable-vtk" ] ; then
       cp Makefile.lammps_vtk8.0 Makefile.lammps
       cd ../vtk
       cp Makefile.lammps_vtk8.0 Makefile.lammps
    elif [ $var == "--enable-hdf5" ]; then
       cp Makefile.lammps_hdf5 Makefile.lammps
    elif [ $var == "--enable-essential" ]; then
       cp Makefile.lammps_essential Makefile.lammps
    elif [ $var == "--enable-vtk-hdf5" ]; then
       cp Makefile.lammps_hdf5_vtk8.0 Makefile.lammps
       cd ../vtk
       cp Makefile.lammps_vtk8.0 Makefile.lammps
    else
       if [ $var != "--serial" ]; then
          echo "Unknown parameter"
          exit 1
       fi
    fi
done


#### Build LAMMPS with NUFEB and VTK packages#####
echo "Installing required packages.."

cd $currentDir/lammps/src
make yes-user-nufeb
make yes-granular
make yes-user-psoriasis

for var in "$@"
do 
    if [ $var == "--enable-vtk" ] || [ $var == "--enable-vtk-hdf5" ]; then
	make yes-user-vtk
    fi
    if [ $var == "--serial" ]; then
	cd STUBS
        make
        cd ..
        make -j4 serial
    else 
        make -j4 mpi
    fi
done

#echo "Writing path to .bashrc"
#echo "export PATH=\$PATH:$currentDir/lammps/src/" >> ~/.bashrc
