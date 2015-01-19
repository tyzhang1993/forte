#include <cmath>

#include <libpsio/psio.hpp>
#include <libmints/wavefunction.h>
#include <libmints/molecule.h>

#include "fcimc.h"
#include "bitset_determinant.h"

using namespace psi;

namespace psi{ namespace libadaptive{

FCIMC::FCIMC(boost::shared_ptr<Wavefunction> wfn, Options &options, ExplorerIntegrals* ints)
    : Wavefunction(options,_default_psio_lib_)
{
    copy(wfn);

    outfile->Printf("\n\n      --------------------------------------");
    outfile->Printf("\n          Full Configuration Interaction");
    outfile->Printf("\n             Quantum Monte Carlo");
    outfile->Printf("\n");
    outfile->Printf("\n                Version 0.1.0");
    outfile->Printf("\n");
    outfile->Printf("\n       written by Francesco A. Evangelista");
    outfile->Printf("\n      --------------------------------------\n");

    BitsetDeterminant::set_ints(ints);
}

FCIMC::~FCIMC()
{
}

double FCIMC::compute_energy()
{
    // Build the reference determinant and compute its energy
    std::vector<bool> occupation_a(nmo_,false);
    std::vector<bool> occupation_b(nmo_,false);
    int cumidx = 0;
    for (int h = 0; h < nirrep_; ++h){
        for (int i = 0; i < doccpi_[h]; ++i){
            occupation_a[i + cumidx] = true;
            occupation_b[i + cumidx] = true;
        }
        for (int i = 0; i < soccpi_[h]; ++i){
            occupation_a[i + doccpi_[h] + cumidx] = true;
        }
        cumidx += nmopi_[h];
    }
    BitsetDeterminant reference(occupation_a,occupation_b);
    BitsetDeterminant excited(occupation_a,occupation_b);
    reference.print();

    double nre = molecule_->nuclear_repulsion_energy();

    double energy1 = reference.energy() + nre;
    double energy2 = reference.slater_rules(reference) + nre;
    outfile->Printf("\n  Energy 1: %f",energy1);
    outfile->Printf("\n  Energy 2: %f",energy2);

    excited.set_alfa_bit(1,false);
    excited.set_beta_bit(1,false);
    excited.set_alfa_bit(6,true);
    excited.set_beta_bit(6,true);

    excited.print();

    std::vector<BitsetDeterminant> walkers;
    walkers.push_back(reference);
    walkers.push_back(excited);
    size_t nwalkers = walkers.size();

    Matrix H("Hamiltonian",nwalkers,nwalkers);
    Matrix evecs("Eigenvectors",nwalkers,nwalkers);
    Vector evals("Eigenvalues",nwalkers);

    for (size_t I = 0; I < nwalkers; ++I){
        for (size_t J = 0; J < nwalkers; ++J){
            double HIJ = walkers[I].slater_rules(walkers[J]) + (I == J ? nre : 0.0);
            H.set(I,J,HIJ);
        }
    }
    H.print();

    H.diagonalize(evecs,evals);
    evecs.print();
    evals.print();

    return 0.0;
}

}} // EndNamespaces
