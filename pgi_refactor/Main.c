#include "XSbench_header.h"

#ifdef MPI
#include<mpi.h>
#endif

int main( int argc, char* argv[] )
{
  // =====================================================================
  // Initialization & Command Line Read-In
  // =====================================================================
  int version = 13;
  int mype = 0;
#ifndef ACC
  int max_procs = omp_get_num_procs();
#endif
  int i, thread, mat;
  unsigned long seed;
  double tick, tock, p_energy;
  unsigned long long vhash = 0;
  int nprocs;

  //Inputs
  int nthreads;
  long n_isotopes;
  long n_gridpoints;
  int lookups;
  char HM[6];

#ifdef MPI
  MPI_Status stat;
  MPI_Init(&argc, &argv);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  MPI_Comm_rank(MPI_COMM_WORLD, &mype);
#endif

  // rand() is only used in the serial initialization stages.
  // A custom RNG is used in parallel portions.
#ifdef VERIFICATION
  srand(26);
#else
  srand(time(NULL));
#endif

  // Process CLI Fields
  read_CLI(argc, argv, &nthreads, &n_isotopes, &n_gridpoints, &lookups, HM);

#ifndef ACC
  // Set number of OpenMP Threads
  omp_set_num_threads(nthreads); 
#endif

  // Print-out of Input Summary
  if( mype == 0 )
    print_inputs(nthreads, n_isotopes, n_gridpoints, lookups, HM, nprocs, version);

  // =====================================================================
  // Prepare Nuclide Energy Grids, Unionized Energy Grid, & Material Data
  // =====================================================================

  // Allocate & fill energy grids
#ifndef BINARY_READ
  if( mype == 0) printf("Generating Nuclide Energy Grids...\n");
#endif

  NuclideGridPoint ** nuclide_grids = gpmatrix(n_isotopes,n_gridpoints);

#ifdef VERIFICATION
  generate_grids_v( nuclide_grids, n_isotopes, n_gridpoints );	
#else
  generate_grids( nuclide_grids, n_isotopes, n_gridpoints );	
#endif

  // Sort grids by energy
#ifndef BINARY_READ
  if( mype == 0) printf("Sorting Nuclide Energy Grids...\n");
  sort_nuclide_grids( nuclide_grids, n_isotopes, n_gridpoints );
#endif

  // Prepare Unionized Energy Grid Framework
  int * grid_ptrs = generate_ptr_grid(n_isotopes, n_gridpoints);
#ifndef BINARY_READ
  GridPoint * energy_grid = generate_energy_grid( n_isotopes,
      n_gridpoints, nuclide_grids, grid_ptrs ); 	
#else
  GridPoint * energy_grid = (GridPoint *)malloc( n_isotopes *
      n_gridpoints * sizeof( GridPoint ) );
  for( i = 0; i < n_isotopes*n_gridpoints; i++ )
    energy_grid[i].xs_ptrs = i*n_isotopes;
#endif

  // Double Indexing. Filling in energy_grid with pointers to the
  // nuclide_energy_grids.
#ifndef BINARY_READ
  set_grid_ptrs( energy_grid, nuclide_grids, grid_ptrs, n_isotopes, n_gridpoints );
#endif

#ifdef BINARY_READ
  if( mype == 0 ) printf("Reading data from \"XS_data.dat\" file...\n");
  binary_read(n_isotopes, n_gridpoints, nuclide_grids, energy_grid, grid_ptrs);
#endif

  // Get material data
  if( mype == 0 )
    printf("Loading Mats...\n");

  int size_mats;
  if (n_isotopes == 68) 
    size_mats = 197;
  else
    size_mats = 484;

  int *num_nucs  = load_num_nucs(n_isotopes);
  int *mats_idx  = load_mats_idx(num_nucs);
  int *mats      = load_mats( num_nucs, mats_idx, size_mats, n_isotopes );

#ifdef VERIFICATION
  double *concs = load_concs_v(size_mats);
#else
  double *concs = load_concs(size_mats);
#endif

#ifdef BINARY_DUMP
  if( mype == 0 ) printf("Dumping data to binary file...\n");
  binary_dump(n_isotopes, n_gridpoints, nuclide_grids, energy_grid, grid_ptrs);
  if( mype == 0 ) printf("Binary file \"XS_data.dat\" written! Exiting...\n");
  return 0;
#endif

  // =====================================================================
  // Cross Section (XS) Parallel Lookup Simulation Begins
  // =====================================================================


  if( mype == 0 )
  {
    printf("\n");
    border_print();
    center_print("SIMULATION", 79);
    border_print();
  }

#ifdef ACC
  tick = timer();
#else
  tick = omp_get_wtime();
#endif


  // OpenMP compiler directives - declaring variables as shared or private
#ifdef ACC
#pragma acc data \
  copy(vhash) \
  copyin( \
      n_isotopes, \
      n_gridpoints, \
      lookups, \
      energy_grid[0:n_isotopes*n_gridpoints], \
      nuclide_grids[0:n_isotopes*n_gridpoints], \
      grid_ptrs[0:n_isotopes*n_isotopes*n_gridpoints], \
      mats[0:size_mats], \
      mats_idx[0:12], \
      concs[0:size_mats], \
      num_nucs[0:12] )
#else
#pragma omp parallel default(none) \
  private(i, thread, p_energy, mat, seed) \
  shared( \
    max_procs, \
    nthreads, \
    n_isotopes, \
    n_gridpoints, \
    lookups, \
    energy_grid, \
    nuclide_grids, \
    grid_ptrs, \
    mats, \
    mats_idx, \
    concs, \
    num_nucs, \
    mype, \
    vhash ) 
#endif
  {	

    double macro_xs_vector[5];

    // Initialize RNG seeds for threads
#ifndef ACC
    thread = omp_get_thread_num();
    seed   = (thread+1)*19+17;
#endif

    // XS Lookup Loop
#ifdef ACC
#pragma acc parallel for
#else
#pragma omp for schedule(dynamic)
#endif
    for( i = 0; i < lookups; i++ )
    {
      // Status text
#ifndef ACC
      if( INFO && mype == 0 && thread == 0 && i % 1000 == 0 )
        printf("\rCalculating XS's... (%.0lf%% completed)",
            (i / ( (double)lookups / (double) nthreads ))
            / (double) nthreads * 100.0);
#endif

      // Randomly pick an energy and material for the particle
#ifdef VERIFICATION
#ifndef ACC
#pragma omp critical
      {
        p_energy = rn_v();
        mat      = pick_mat(rn_v()); 
      }
#endif
#else
      p_energy = rn(&seed);
      mat      = pick_mat(rn(&seed)); 
#endif

      // debugging
      //printf("E = %lf mat = %d\n", p_energy, mat);

      // This returns the macro_xs_vector, but we're not going
      // to do anything with it in this program, so return value
      // is written over.
      calculate_macro_xs( p_energy, mat, n_isotopes,
          n_gridpoints, num_nucs, concs,
          energy_grid, grid_ptrs, nuclide_grids, mats, mats_idx,
          macro_xs_vector );

      // Verification hash calculation
      // This method provides a consistent hash accross
      // architectures and compilers.
#ifdef VERIFICATION
      char line[256];
      sprintf(line, "%.5lf %d %.5lf %.5lf %.5lf %.5lf %.5lf",
          p_energy, mat,
          macro_xs_vector[0],
          macro_xs_vector[1],
          macro_xs_vector[2],
          macro_xs_vector[3],
          macro_xs_vector[4]);
      unsigned long long vhash_local = hash(line, 10000);
#ifndef ACC
#pragma omp atomic
#endif
      vhash += vhash_local;
#endif
    }


  }

#ifdef ACC
  tock = timer();
#else
  tock = omp_get_wtime();
#endif

  // Print / Save Results and Exit
  print_results(nthreads, n_isotopes, n_gridpoints, lookups, HM, mype, tock-tick, nprocs, vhash);


#ifdef MPI
  MPI_Finalize();
#endif

  return 0;
}