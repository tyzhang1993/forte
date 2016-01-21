#include <cmath>
#include <functional>
#include <algorithm>

#include <unordered_map>
#include <boost/timer.hpp>
#include <boost/format.hpp>

#include <libciomr/libciomr.h>
#include <libpsio/psio.h>
#include <libpsio/psio.hpp>
#include <libqt/qt.h>
#include <libmints/molecule.h>
#include <libmints/vector.h>

#include "apici.h"
#include "sparse_ci_solver.h"
#include "helpers.h"
#include "dynamic_bitset_determinant.h"
#include "fci_vector.h"
#include <boost/math/special_functions/bessel.hpp>

using namespace std;
using namespace psi;

#define USE_HASH 1
#define DO_STATS 0
#define ENFORCE_SYM 1

namespace psi{ namespace forte{
#ifdef _OPENMP
   #include <omp.h>
   bool AdaptivePathIntegralCI::have_omp_ = true;
#else
   #define omp_get_max_threads() 1
   #define omp_get_thread_num() 0
   bool AdaptivePathIntegralCI::have_omp_ = false;
#endif

void combine_hashes(std::vector<det_hash<>>& thread_det_C_map, det_hash<>& dets_C_hash);
void combine_hashes(det_hash<>& dets_C_hash_A,det_hash<>& dets_C_hash_B);
void combine_hashes_into_hash(std::vector<det_hash<>>& thread_det_C_hash,det_hash<>& dets_C_hash);
void copy_hash_to_vec(det_hash<>& dets_C_hash,det_vec& dets,std::vector<double>& C);
void scale(std::vector<double>& A,double alpha);
void scale(det_hash<>& A,double alpha);
double normalize(std::vector<double>& C);
double normalize(det_hash<>& dets_C);
double norm(det_hash<>& dets_C);
double dot(det_hash<>& A,det_hash<>& B);
void add(det_hash<>& A,double beta,det_hash<>& B);
double factorial(int n);
void binomial_coefs(std::vector<double>& coefs, int order, double a, double b);
void Taylor_propagator_coefs(std::vector<double>& coefs, int order, double tau, double S);
void Taylor_polynomial_coefs(std::vector<double>& coefs, int order);
void Chebyshev_polynomial_coefs(std::vector<double>& coefs, int order);
void Chebyshev_propagator_coefs(std::vector<double>& coefs, int order, double tau, double S, double range);
void print_polynomial(std::vector<double>& coefs);

AdaptivePathIntegralCI::AdaptivePathIntegralCI(boost::shared_ptr<Wavefunction> wfn, Options &options,
                                               std::shared_ptr<ForteIntegrals>  ints, std::shared_ptr<MOSpaceInfo> mo_space_info)
    : Wavefunction(options,_default_psio_lib_),
      options_(options),
      ints_(ints),
      mo_space_info_(mo_space_info),
      prescreening_tollerance_factor_(1.5),
      fast_variational_estimate_(false)
{
    // Copy the wavefunction information
    copy(wfn);
    startup();
}


std::shared_ptr<FCIIntegrals> AdaptivePathIntegralCI::fci_ints_ = 0;

void AdaptivePathIntegralCI::startup()
{
    // Connect the integrals to the determinant class
    fci_ints_ = std::make_shared<FCIIntegrals>(ints_, mo_space_info_->get_corr_abs_mo("ACTIVE"), mo_space_info_->get_corr_abs_mo("RESTRICTED_DOCC"));

    auto active_mo = mo_space_info_->get_corr_abs_mo("ACTIVE");
    ambit::Tensor tei_active_aa = ints_->aptei_aa_block(active_mo, active_mo, active_mo, active_mo);
    ambit::Tensor tei_active_ab = ints_->aptei_ab_block(active_mo, active_mo, active_mo, active_mo);
    ambit::Tensor tei_active_bb = ints_->aptei_bb_block(active_mo, active_mo, active_mo, active_mo);
    fci_ints_->set_active_integrals(tei_active_aa, tei_active_ab, tei_active_bb);
    fci_ints_->compute_restricted_one_body_operator();

    Determinant::set_ints(fci_ints_);
 //   DynamicBitsetDeterminant::set_ints(fci_ints_);

    // The number of correlated molecular orbitals
    ncmo_ = mo_space_info_->get_corr_abs_mo("ACTIVE").size();
    ncmopi_ = mo_space_info_->get_dimension("ACTIVE");

    // Overwrite the frozen orbitals arrays
    frzcpi_ = mo_space_info_->get_dimension("FROZEN_DOCC");
    frzvpi_ = mo_space_info_->get_dimension("FROZEN_UOCC");

    nuclear_repulsion_energy_ = molecule_->nuclear_repulsion_energy();

    // Create the array with mo symmetry
    for (int h = 0; h < nirrep_; ++h){
        for (int p = 0; p < ncmopi_[h]; ++p){
            mo_symmetry_.push_back(h);
        }
    }

    wavefunction_symmetry_ = 0;
    if(options_["ROOT_SYM"].has_changed()){
        wavefunction_symmetry_ = options_.get_int("ROOT_SYM");
    }

    // Build the reference determinant and compute its energy
    std::vector<int> occupation(2 * ncmo_,0);
    int cumidx = 0;
    for (int h = 0; h < nirrep_; ++h){
        for (int i = 0; i < doccpi_[h] - frzcpi_[h]; ++i){
            occupation[i + cumidx] = 1;
            occupation[ncmo_ + i + cumidx] = 1;
        }
        for (int i = 0; i < soccpi_[h]; ++i){
            occupation[i + cumidx + doccpi_[h] - frzcpi_[h]] = 1;
        }
        cumidx += ncmopi_[h];
    }
    reference_determinant_ = Determinant(occupation);

//    outfile->Printf("\n  The reference determinant is:\n");
//    reference_determinant_.print();

    // Read options
    wavefunction_multiplicity_ = 1;
    if(options_["MULTIPLICITY"].has_changed()){
        wavefunction_multiplicity_ = options_.get_int("MULTIPLICITY");
    }
    nroot_ = options_.get_int("NROOT");
    current_root_ = -1;
    post_diagonalization_ = false;
//    /-> Define appropriate variable: post_diagonalization_ = options_.get_bool("EX_ALGORITHM");

    spawning_threshold_ = options_.get_double("SPAWNING_THRESHOLD");
    initial_guess_spawning_threshold_ = options_.get_double("GUESS_SPAWNING_THRESHOLD");
    if (initial_guess_spawning_threshold_ < 0.0) initial_guess_spawning_threshold_ = 10.0*spawning_threshold_;
    time_step_ = options_.get_double("TAU");
    maxiter_ = options_.get_int("MAXBETA") / time_step_;
    e_convergence_ = options_.get_double("E_CONVERGENCE");
    energy_estimate_threshold_ = options_.get_double("ENERGY_ESTIMATE_THRESHOLD");
    initiator_approx_factor_ = options_.get_double("INITIATOR_APPROX_FACTOR");

    max_guess_size_ = options_.get_int("MAX_GUESS_SIZE");
    energy_estimate_freq_ = options_.get_int("ENERGY_ESTIMATE_FREQ");

    adaptive_beta_ = options_.get_bool("ADAPTIVE_BETA");
    fast_variational_estimate_ = options_.get_bool("FAST_EVAR");
    do_shift_ = options_.get_bool("USE_SHIFT");
    use_inter_norm_ = options_.get_bool("USE_INTER_NORM");
    do_simple_prescreening_ = options_.get_bool("SIMPLE_PRESCREENING");
    do_dynamic_prescreening_ = options_.get_bool("DYNAMIC_PRESCREENING");
    do_schwarz_prescreening_ = options_.get_bool("SCHWARZ_PRESCREENING");
    do_initiator_approx_ = options_.get_bool("INITIATOR_APPROX");
    chebyshev_order_ = options_.get_int("CHEBYSHEV_ORDER");

    if (options_.get_str("PROPAGATOR") == "LINEAR"){
        propagator_ = LinearPropagator;
        propagator_description_ = "Linear";
    }else if (options_.get_str("PROPAGATOR") == "TROTTER"){
        propagator_ = TrotterLinear;
        propagator_description_ = "Trotter";
    }else if (options_.get_str("PROPAGATOR") == "QUADRATIC"){
        propagator_ = QuadraticPropagator;
        propagator_description_ = "Quadratic";
    }else if (options_.get_str("PROPAGATOR") == "CUBIC"){
        propagator_ = CubicPropagator;
        propagator_description_ = "Cubic";
    }else if (options_.get_str("PROPAGATOR") == "QUARTIC"){
        propagator_ = QuarticPropagator;
        propagator_description_ = "Quartic";
    }else if (options_.get_str("PROPAGATOR") == "POWER"){
        propagator_ = PowerPropagator;
        propagator_description_ = "Power";
    }else if (options_.get_str("PROPAGATOR") == "OLSEN"){
        propagator_ = OlsenPropagator;
        propagator_description_ = "Olsen";
        // Make sure that do_shift_ is set to true
        do_shift_ = true;
    }else if (options_.get_str("PROPAGATOR") == "DAVIDSON"){
        propagator_ = DavidsonLiuPropagator;
        propagator_description_ = "Davidson-Liu";
        // Make sure that do_shift_ is set to true
        do_shift_ = true;
    }else if (options_.get_str("PROPAGATOR") == "CHEBYSHEV"){
        propagator_ = ChebyshevPropagator;
        propagator_description_ = "Chebyshev";
        // Make sure that do_shift_ is set to true
//        do_shift_ = true;
    }

    num_threads_ = omp_get_max_threads();
}

void AdaptivePathIntegralCI::print_info()
{
    // Print a summary
    std::vector<std::pair<std::string,int>> calculation_info{
        {"Symmetry",wavefunction_symmetry_},
        {"Multiplicity",wavefunction_multiplicity_},
        {"Number of roots",nroot_},
        {"Root used for properties",options_.get_int("ROOT")},
        {"Maximum number of iterations",maxiter_},
        {"Energy estimation frequency",energy_estimate_freq_},
        {"Number of threads",num_threads_}};

    std::vector<std::pair<std::string,double>> calculation_info_double{
        {"Time step (beta)",time_step_},
        {"Spawning threshold",spawning_threshold_},
        {"Initial guess spawning threshold",initial_guess_spawning_threshold_},
        {"Convergence threshold",e_convergence_},
        {"Prescreening tollerance factor",prescreening_tollerance_factor_},
        {"Energy estimate tollerance",energy_estimate_threshold_}};

    std::vector<std::pair<std::string,std::string>> calculation_info_string{
        {"Propagator type",propagator_description_},        
        {"Adaptive time step",adaptive_beta_ ? "YES" : "NO"},
        {"Shift the energy",do_shift_ ? "YES" : "NO"},
        {"Use intermediate normalization", use_inter_norm_ ? "YES" : "NO"},
        {"Prescreen spawning",do_simple_prescreening_ ? "YES" : "NO"},
        {"Dynamic prescreening",do_dynamic_prescreening_ ? "YES" : "NO"},
        {"Schwarz prescreening",do_schwarz_prescreening_ ? "YES" : "NO"},
        {"Initiator approximation",do_initiator_approx_ ? "YES" : "NO"},
        {"Fast variational estimate",fast_variational_estimate_ ? "YES" : "NO"},
        {"Using OpenMP", have_omp_ ? "YES" : "NO"},
    };
//    {"Number of electrons",nel},
//    {"Number of correlated alpha electrons",nalpha_},
//    {"Number of correlated beta electrons",nbeta_},
//    {"Number of restricted docc electrons",rdoccpi_.sum()},
//    {"Charge",charge},
//    {"Multiplicity",multiplicity},

    // Print some information
    outfile->Printf("\n\n  ==> Calculation Information <==\n");
    for (auto& str_dim : calculation_info){
        outfile->Printf("\n    %-39s %10d",str_dim.first.c_str(),str_dim.second);
    }
    for (auto& str_dim : calculation_info_double){
        outfile->Printf("\n    %-39s %10.3e",str_dim.first.c_str(),str_dim.second);
    }
    for (auto& str_dim : calculation_info_string){
        outfile->Printf("\n    %-39s %10s",str_dim.first.c_str(),str_dim.second.c_str());
    }
    outfile->Flush();
}

void print_polynomial(std::vector<double>& coefs) {
    outfile->Printf("\n    f(x) = ");
    for (int i=coefs.size()-1; i >= 0; i--) {
        switch (i) {
        case 0:
            outfile->Printf("%s%e", coefs[i] >= 0 ? "+" : "", coefs[i]);
            break;
        case 1:
            outfile->Printf("%s%e * x ", coefs[i] >= 0 ? "+" : "", coefs[i]);
            break;
        default:
            outfile->Printf("%s%e * x^%d ", coefs[i] >= 0 ? "+" : "", coefs[i], i);
            break;
        }
    }
}

void AdaptivePathIntegralCI::print_characteristic_function(double tau, double S, double lambda_1, double lambda_2, double lambda_h)
{
    std::vector<double> coefs;
    switch (propagator_) {
    case PowerPropagator:
        coefs.push_back(-S);
        coefs.push_back(1.0);
        break;
    case LinearPropagator:
        Taylor_propagator_coefs(coefs, 1, tau, S);
        break;
    case QuadraticPropagator:
        Taylor_propagator_coefs(coefs, 2, tau, S);
        break;
    case CubicPropagator:
        Taylor_propagator_coefs(coefs, 3, tau, S);
        break;
    case QuarticPropagator:
        Taylor_propagator_coefs(coefs, 4, tau, S);
        break;
    case ChebyshevPropagator:
        Chebyshev_propagator_coefs(coefs, chebyshev_order_, tau, S, range_);
        break;
    default:
        break;
    }
    outfile->Printf("\n\n  ==> Characteristic Function <==");
    print_polynomial(coefs);
    outfile->Printf("\n    with tau = %e, shift = %.12f", tau, S);
    if (propagator_ == ChebyshevPropagator)
        outfile->Printf(", range = %.12f", range_);
    outfile->Printf("\n    Initial guess: lambda_1= %s%.12f", lambda_1 >= 0.0 ? " " : "", lambda_1);
    outfile->Printf("\n                   lambda_2= %s%.12f", lambda_2 >= 0.0 ? " " : "", lambda_2);
    outfile->Printf("\n    Est. Highest eigenvalue= %s%.12f", lambda_h >= 0.0 ? " " : "", lambda_h);
}

double factorial(int n)
{
    return (n == 1 || n == 0) ? 1.0 : factorial(n - 1) * n;
}

void binomial_coefs(std::vector<double>& coefs, int order, double a, double b) {
    coefs.clear();
    for (int i = 0; i <= order; i++) {
        coefs.push_back(factorial(order)/(factorial(i)*factorial(order-i))*pow(a, i)*pow(b, order-i));
    }
}

void Polynomial_propagator_coefs(std::vector<double>& coefs, std::vector<double>& poly_coefs, double a, double b) {
    coefs.clear();
    int order = poly_coefs.size() - 1;
    for (int i = 0; i <= order; i++) {
        coefs.push_back(0.0);
    }
    for (int i = 0; i <= order; i++) {
        std::vector<double> bino_coefs;
        binomial_coefs(bino_coefs, i, a, b);
        for (int j = 0; j <= i; j++) {
            coefs[j] += poly_coefs[i] * bino_coefs[j];
        }
    }
}

void Taylor_polynomial_coefs(std::vector<double>& coefs, int order) {
    coefs.clear();
    for (int i=0; i <= order; i++) {
        coefs.push_back(1.0/factorial(i));
    }
}

void Taylor_propagator_coefs(std::vector<double>& coefs, int order, double tau, double S) {
    coefs.clear();
    std::vector<double> poly_coefs;
    Taylor_polynomial_coefs(poly_coefs, order);
    Polynomial_propagator_coefs(coefs, poly_coefs, -tau, tau * S);
//    coefs.clear();
//    for (int i=0; i <= order; i++) {
//        coefs.push_back(0.0);
//        for (int j=0; j <= i; j++) {
//            coefs[j] += 1.0/(factorial(j)*factorial(i-j))*pow(-tau, i)*pow(-S, i-j);
//        }
//    }
}

void Chebyshev_polynomial_coefs(std::vector<double>& coefs, int order) {
    coefs.clear();
    std::vector<double> coefs_0, coefs_1;
    if (order == 0) {
        coefs.push_back(1.0);
        return;
    }
    else
        coefs_0.push_back(1.0);
    if (order == 1) {
        coefs.push_back(0.0);
        coefs.push_back(1.0);
        return;
    }
    else{
        coefs_1.push_back(0.0);
        coefs_1.push_back(1.0);
    }
    for (int i = 2; i <= order; i++) {
        coefs.clear();
        for (int j = 0; j <= i; j++){
            coefs.push_back(0.0);
        }
        for (int j = 0; j <= i-2; j++) {
            coefs[j] -= coefs_0[j];
        }
        for (int j = 0; j <= i-1; j++) {
            coefs[j+1] += 2.0 * coefs_1[j];
        }
        coefs_0 = coefs_1;
        coefs_1 = coefs;
    }
}

void Chebyshev_propagator_coefs(std::vector<double>& coefs, int order, double tau, double S, double range) {
    coefs.clear();
    std::vector<double> poly_coefs;
    for (int i = 0; i <= order; i++) {
        poly_coefs.push_back(0.0);
    }

    for (int i = 0; i <= order; i++) {
        std::vector<double> chbv_poly_coefs;
        Chebyshev_polynomial_coefs(chbv_poly_coefs, i);
//        outfile->Printf("\n\n  chebyshev poly in step %d", i);
//        print_polynomial(chbv_poly_coefs);
        for (int j = 0; j <= i; j++) {
            poly_coefs[j] += (i == 0 ? 1.0 : 2.0) * boost::math::cyl_bessel_i(i, range) * chbv_poly_coefs[j];
        }
//        outfile->Printf("\n\n  propagate poly in step %d", i);
//        print_polynomial(poly_coefs);
    }
    Polynomial_propagator_coefs(coefs, poly_coefs, -tau/range, tau * S/range);
}

double AdaptivePathIntegralCI::compute_energy()
{
    timer_on("PIFCI:Energy");
    boost::timer t_apici;
    old_max_one_HJI_ = 1e100;
    new_max_one_HJI_ = 1e100;
    old_max_two_HJI_ = 1e100;
    new_max_two_HJI_ = 1e100;

    // Increase the root counter (ground state = 0)
    current_root_ += 1;

    outfile->Printf("\n\n\t  ---------------------------------------------------------");
    outfile->Printf("\n\t      Adaptive Path-Integral Full Configuration Interaction");
    outfile->Printf("\n\t                   by Francesco A. Evangelista");
    outfile->Printf("\n\t                    %4d thread(s) %s",num_threads_,have_omp_ ? "(OMP)" : "");
    outfile->Printf("\n\t  ---------------------------------------------------------");

    // Print a summary of the options
    print_info();

    /// A vector of determinants in the P space
    det_vec dets;
    std::vector<double> C;

    SparseCISolver sparse_solver;
    sparse_solver.set_parallel(true);

    pqpq_aa_ = new double[ncmo_*ncmo_];
    pqpq_ab_ = new double[ncmo_*ncmo_];
    pqpq_bb_ = new double[ncmo_*ncmo_];

    for (size_t i=0; i < (size_t)ncmo_; ++i) {
////        pqpq_row_max_.push_back(0.0);
        for (size_t j=0; j < (size_t)ncmo_; ++j) {
            double temp_aa = sqrt(fabs(fci_ints_->tei_aa(i,j,i,j)));
            pqpq_aa_[i*ncmo_+j] = temp_aa;
            if (temp_aa > pqpq_max_aa_)
                pqpq_max_aa_=temp_aa;
            double temp_ab = sqrt(fabs(fci_ints_->tei_ab(i,j,i,j)));
            pqpq_ab_[i*ncmo_+j] = temp_ab;
            if (temp_ab > pqpq_max_ab_)
                pqpq_max_ab_=temp_ab;
            double temp_bb = sqrt(fabs(fci_ints_->tei_bb(i,j,i,j)));
            pqpq_bb_[i*ncmo_+j] = temp_bb;
            if (temp_bb > pqpq_max_bb_)
                pqpq_max_bb_=temp_bb;
////            if (temp_aa > pqpq_row_max_[i])
////                pqpq_row_max_[i]=temp;
        }
    }

//    Determinant detI(reference_determinant_);

//    std::vector<int> aocc = detI.get_alfa_occ();
//    std::vector<int> bocc = detI.get_beta_occ();
//    std::vector<int> avir = detI.get_alfa_vir();
//    std::vector<int> bvir = detI.get_beta_vir();
//    std::vector<int> aocc_offset(nirrep_ + 1);
//    std::vector<int> bocc_offset(nirrep_ + 1);
//    std::vector<int> avir_offset(nirrep_ + 1);
//    std::vector<int> bvir_offset(nirrep_ + 1);

//    int noalpha = aocc.size();
//    int nobeta  = bocc.size();
//    int nvalpha = avir.size();
//    int nvbeta  = bvir.size();

//    for (int i = 0; i < noalpha; ++i) aocc_offset[mo_symmetry_[aocc[i]] + 1] += 1;
//    for (int a = 0; a < nvalpha; ++a) avir_offset[mo_symmetry_[avir[a]] + 1] += 1;
//    for (int i = 0; i < nobeta; ++i) bocc_offset[mo_symmetry_[bocc[i]] + 1] += 1;
//    for (int a = 0; a < nvbeta; ++a) bvir_offset[mo_symmetry_[bvir[a]] + 1] += 1;
//    for (int h = 1; h < nirrep_ + 1; ++h){
//        aocc_offset[h] += aocc_offset[h-1];
//        avir_offset[h] += avir_offset[h-1];
//        bocc_offset[h] += bocc_offset[h-1];
//        bvir_offset[h] += bvir_offset[h-1];
//    }

//    for (int h = 0; h < nirrep_; ++h){
//        for (int i = aocc_offset[h], a = avir_offset[h + 1] - 1 ; i < aocc_offset[h + 1] && a >= avir_offset[h] ; ++i, --a){
//            int ii = aocc[i];
//            int aa = avir[a];
//            detI.set_alfa_bit(ii,false);
//            detI.set_alfa_bit(aa,true);
//        }
//        for (int i = bocc_offset[h], a = bvir_offset[h + 1] - 1 ; i < bocc_offset[h + 1] && a >= bvir_offset[h] ; ++i, --a){
//            int ii = bocc[i];
//            int aa = bvir[a];
//            detI.set_beta_bit(ii,false);
//            detI.set_beta_bit(aa,true);
//        }
//    }
//    double ref_energy = reference_determinant_.energy();
//    outfile->Printf("\nreference energy:%.12lf", ref_energy);
//    reference_determinant_.print();
//    detI.print();
//    double max_energy = detI.energy();
//    outfile->Printf("\nmax_excit energy:%.12lf", max_energy);
//    double power_shift = 5./8. * max_energy + 3./8. * ref_energy;

    double high_obt_energy = 0.0;
    int ne = 0;
    auto bits_ = reference_determinant_.bits_;
    for (int i = 0; i < ncmo_; i++) {
        if (bits_[i]) ++ne;
        if (bits_[nmo_ +i]) ++ne;

        double temp  = fci_ints_->oei_a(i,i);
        for(int p = 0; p < ncmo_; ++p){
            if(bits_[p]){
                temp += fci_ints_->tei_aa(i,p,i,p);
            }
            if(bits_[nmo_ +p]){
                temp += fci_ints_->tei_ab(i,p,i,p);
            }
        }
        if (temp > high_obt_energy) high_obt_energy = temp;
    }
//    outfile->Printf("\nhigh obt energy:%.12lf", high_obt_energy);

    double ref_energy = reference_determinant_.energy();
//    outfile->Printf("\nreference energy:%.12lf", ref_energy);
//    reference_determinant_.print();
    double max_energy = high_obt_energy * ne;
//    outfile->Printf("\nmax_excit energy:%.12lf", max_energy);
    double power_shift = 5./8. * max_energy + 3./8. * ref_energy;
    range_ = (power_shift-ref_energy)*1.2*time_step_;
//    outfile->Printf("\nshift:%.12lf\trange:%.12f", power_shift, range_);

    // Compute the initial guess
    outfile->Printf("\n\n  ==> Initial Guess <==");
    double var_energy = initial_guess(dets,C);
    double proj_energy = var_energy;

    print_wfn(dets,C);
    det_hash<> old_space_map;
    for (size_t I = 0; I < dets.size(); ++I){
        old_space_map[dets[I]] = C[I];
    }

    if (propagator_ == PowerPropagator || propagator_ == ChebyshevPropagator) {
        print_characteristic_function(time_step_, power_shift, var_energy, 0.0, max_energy);
    } else {
        print_characteristic_function(time_step_, 0.0, var_energy, 0.0, max_energy);
    }


    // Main iterations
    outfile->Printf("\n\n  ==> APIFCI Iterations <==");

    outfile->Printf("\n\n  ------------------------------------------------------------------------------------------");
    outfile->Printf("\n    Steps  Beta/Eh      Ndets     Proj. Energy/Eh  |dEp/dt|      Var. Energy/Eh   |dEv/dt|");
    outfile->Printf("\n  ------------------------------------------------------------------------------------------");

    int maxcycle = maxiter_;
    double old_var_energy = 0.0;
    double old_proj_energy = 0.0;
    double beta = 0.0;
    bool converged = false;

    schwarz_succ_=0;
    schwarz_total_=0;

    for (int cycle = 0; cycle < maxcycle; ++cycle){
        iter_ = cycle;
        double shift = do_shift_ ? var_energy - nuclear_repulsion_energy_ : 0.0;

        if (propagator_ == PowerPropagator || propagator_ == ChebyshevPropagator) {
            shift = power_shift;
        }

        // Compute |n+1> = exp(-tau H)|n>
        timer_on("PIFCI:Step");
        if (use_inter_norm_) {
            auto minmax_C = std::minmax_element(C.begin(),C.end());
            double min_C_abs = fabs(*minmax_C.first);
            double max_C = *minmax_C.second;
            max_C = max_C > min_C_abs ? max_C : min_C_abs;
            propagate(propagator_,dets,C,time_step_,spawning_threshold_ * max_C,shift);
        } else {
            propagate(propagator_,dets,C,time_step_,spawning_threshold_,shift);
        }
        timer_off("PIFCI:Step");
        if (propagator_ == DavidsonLiuPropagator) break;

        // Orthogonalize this solution with respect to the previous ones
        timer_on("PIFCI:Ortho");
        if (current_root_ > 0){
            orthogonalize(dets,C,solutions_);
        }
        timer_off("PIFCI:Ortho");

        // Compute the energy and check for convergence
        if (cycle % energy_estimate_freq_ == 0){
            timer_on("PIFCI:<E>");
            std::map<std::string,double> results = estimate_energy(dets,C);
            timer_off("PIFCI:<E>");

            var_energy = results["VARIATIONAL ENERGY"];
            proj_energy = results["PROJECTIVE ENERGY"];

            double var_energy_gradient = std::fabs((var_energy - old_var_energy) / (time_step_ * energy_estimate_freq_));
            double proj_energy_gradient = std::fabs((proj_energy - old_proj_energy) / (time_step_ * energy_estimate_freq_));

            outfile->Printf("\n%9d %8.2f %10zu %20.12f %.3e %20.12f %.3e",cycle,beta,C.size(),
                            proj_energy,proj_energy_gradient,
                            var_energy,var_energy_gradient);

            old_var_energy = var_energy;
            old_proj_energy = proj_energy;

            if (std::fabs(var_energy_gradient) < e_convergence_){
                converged = true;
                break;
            }
        }
        beta += time_step_;
        outfile->Flush();
    }

    outfile->Printf("\n  ------------------------------------------------------------------------------------------");
    outfile->Printf("\n\n  Calculation %s",converged ? "converged." : "did not converge!");

    if (fast_variational_estimate_){
        var_energy = estimate_var_energy_sparse(dets,C,1.0e-14);
    }else{
        var_energy = estimate_var_energy(dets,C,1.0e-14);
    }

    Process::environment.globals["APIFCI ENERGY"] = var_energy;

    outfile->Printf("\n\n  ==> Post-Iterations <==\n");
    outfile->Printf("\n  * Adaptive-CI Variational Energy     = %.12f Eh",1,var_energy);
    outfile->Printf("\n  * Adaptive-CI Projective  Energy     = %.12f Eh",1,proj_energy);

    outfile->Printf("\n\n  * Size of CI space                   = %zu",C.size());
    if (do_schwarz_prescreening_) {
        outfile->Printf("\n  * Schwarz prescreening total attempt= %zu",schwarz_total_);
        outfile->Printf("\n  * Schwarz prescreening succeed      = %zu",schwarz_succ_);
    }

    outfile->Printf("\n\n  %s: %f s","Adaptive Path-Integral CI (bitset) ran in ",t_apici.elapsed());
    outfile->Flush();

    print_wfn(dets,C);
    if (current_root_ < nroot_ - 1){
        save_wfn(dets,C,solutions_);
    }

    if (post_diagonalization_){
        SharedMatrix apfci_evecs;
        SharedVector apfci_evals;
//        sparse_solver.diagonalize_hamiltonian(dets,apfci_evals,apfci_evecs,nroot_,DavidsonLiuList);
    }

    delete [] pqpq_aa_;
    delete [] pqpq_ab_;
    delete [] pqpq_bb_;

    timer_off("PIFCI:Energy");
    return var_energy;
}


double AdaptivePathIntegralCI::initial_guess(det_vec& dets,std::vector<double>& C)
{
    // Use the reference determinant as a starting point
    std::vector<bool> alfa_bits = reference_determinant_.get_alfa_bits_vector_bool();
    std::vector<bool> beta_bits = reference_determinant_.get_beta_bits_vector_bool();
    det_hash<> dets_C;

    // Do one time step starting from the reference determinant
    Determinant bs_det(alfa_bits,beta_bits);
    det_vec guess_dets{bs_det};

    apply_tau_H(time_step_,initial_guess_spawning_threshold_,guess_dets,{1.0},dets_C,0.0);

    // Save the list of determinants
    copy_hash_to_vec(dets_C,dets,C);

    size_t guess_size = dets.size();
    if (guess_size > max_guess_size_){
        // Consider the 1000 largest contributions
        std::vector<std::pair<double,size_t> > det_weight;
        for (size_t I = 0, max_I = C.size(); I < max_I; ++I){
            det_weight.push_back(std::make_pair(std::fabs(C[I]),I));
            //dets[I].print();
        }
        std::sort(det_weight.begin(),det_weight.end());
        std::reverse(det_weight.begin(),det_weight.end());

        det_vec new_dets;
        for (size_t sI = 0; sI < max_guess_size_; ++sI){
            size_t I = det_weight[sI].second;
            new_dets.push_back(dets[I]);
        }
        dets = new_dets;
        C.resize(guess_size);
        guess_size = dets.size();
    }


    outfile->Printf("\n\n  Initial guess size = %zu",guess_size);

    SparseCISolver sparse_solver;
    sparse_solver.set_parallel(true);

    SharedMatrix evecs(new Matrix("Eigenvectors",guess_size,nroot_));
    SharedVector evals(new Vector("Eigenvalues",nroot_));
  //  std::vector<DynamicBitsetDeterminant> dyn_dets;
   // for (auto& d : dets){
     //   DynamicBitsetDeterminant dbs = d.to_dynamic_bitset();
      //  dyn_dets.push_back(dbs);
   // }
    sparse_solver.diagonalize_hamiltonian(dets,evals,evecs,nroot_,wavefunction_multiplicity_,DavidsonLiuList);
    double var_energy = evals->get(current_root_) + nuclear_repulsion_energy_;
    outfile->Printf("\n\n  Initial guess energy (variational) = %20.12f Eh (root = %d)",var_energy,current_root_ + 1);

    // Copy the ground state eigenvector
    for (size_t I = 0; I < guess_size; ++I){
        C[I] = evecs->get(I,current_root_);
    }
    return var_energy;
}

void AdaptivePathIntegralCI::propagate(PropagatorType propagator, det_vec& dets, std::vector<double>& C, double tau, double spawning_threshold, double S)
{
    // Reset prescreening boundary
    if (do_simple_prescreening_){
        new_max_one_HJI_ = 0.0;
        new_max_two_HJI_ = 0.0;
    }

    // Evaluate (1-beta H) |C>
    if (propagator == LinearPropagator){
        propagate_first_order(dets,C,tau,spawning_threshold,S);
    }else if (propagator == TrotterLinear){
        propagate_Trotter_linear(dets,C,tau,spawning_threshold,S);
    }else if (propagator == ChebyshevPropagator){
        propagate_Chebyshev(dets,C,tau,spawning_threshold,S);
    }else if (propagator == QuadraticPropagator){
        propagate_Taylor(2,dets,C,tau,spawning_threshold,S);
    }else if (propagator == CubicPropagator){
        propagate_Taylor(3,dets,C,tau,spawning_threshold,S);
    }else if (propagator == QuarticPropagator){
        propagate_Taylor(4,dets,C,tau,spawning_threshold,S);
    }else if (propagator == PowerPropagator){
        propagate_power(dets,C,spawning_threshold,S);
    }else if (propagator == OlsenPropagator){
        propagate_Olsen(dets,C,spawning_threshold,S);
    }else if (propagator == DavidsonLiuPropagator){
        propagate_DavidsonLiu(dets,C,spawning_threshold);
    }

    // Update prescreening boundary
    if (do_simple_prescreening_){
        old_max_one_HJI_ = new_max_one_HJI_;
        old_max_two_HJI_ = new_max_two_HJI_;
    }
    normalize(C);
}


void AdaptivePathIntegralCI::propagate_first_order(det_vec& dets,std::vector<double>& C,double tau,double spawning_threshold,double S)
{
    // A map that contains the pair (determinant,coefficient)
    det_hash<> dets_C_hash;

    // Term 1. |n>
    for (size_t I = 0, max_I = dets.size(); I < max_I; ++I){
        dets_C_hash[dets[I]] = C[I];
    }
    // Term 2. -tau (H - S)|n>
    apply_tau_H(-tau,spawning_threshold,dets,C,dets_C_hash,S);

    // Overwrite the input vectors with the updated wave function
    copy_hash_to_vec(dets_C_hash,dets,C);
}

void AdaptivePathIntegralCI::propagate_Taylor(int order,det_vec& dets,std::vector<double>& C,double tau,double spawning_threshold,double S)
{
    // A map that contains the pair (determinant,coefficient)
    det_hash<> dets_C_hash;
    det_hash<> dets_sum_map;
    // A vector of maps that hold (determinant,coefficient)

    // Propagate the wave function for one time step using |n+1> = (1 - tau (H-S) + tau^2 (H-S)^2 / 2)|n>

    // Term 1. |n>
    for (size_t I = 0, max_I = dets.size(); I < max_I; ++I){
        dets_sum_map[dets[I]] = C[I];
    }

    for (int j = 1; j <= order; ++j){
        double delta_tau = -tau/ double(j);
        apply_tau_H(delta_tau,spawning_threshold,dets,C,dets_C_hash,S);

        // Add this term to the total vector
        combine_hashes(dets_C_hash,dets_sum_map);
        // Copy the wave function to a vector
        if (j < order){
            copy_hash_to_vec(dets_C_hash,dets,C);
        }
        dets_C_hash.clear();
//        if(iter_ % energy_estimate_freq_ == 0){
//            double norm = 0.0;
//            for (double CI : C) norm += CI * CI;
//            norm = std::sqrt(norm);
//            outfile->Printf("\n  ||C(%d)-C(%d)|| = %e (%f)",j,j-1,norm,delta_tau);
//        }
    }
    copy_hash_to_vec(dets_sum_map,dets,C);
}

void AdaptivePathIntegralCI::propagate_power(det_vec& dets,std::vector<double>& C,double spawning_threshold,double S)
{
    // A map that contains the pair (determinant,coefficient)
    det_hash<> dets_C_hash;

    apply_tau_H(1.0,spawning_threshold,dets,C,dets_C_hash,S);

    // Overwrite the input vectors with the updated wave function
    copy_hash_to_vec(dets_C_hash,dets,C);
}

void AdaptivePathIntegralCI::propagate_Chebyshev(det_vec& dets,std::vector<double>& C,double tau,double spawning_threshold,double S)
{
    // A map that contains the pair (determinant,coefficient)
    det_hash<> dets_C_hash;
    det_hash<> spawned;
    for (size_t I = 0, max_I = dets.size(); I < max_I; ++I){
        dets_C_hash[dets[I]] = C[I];
    }
    det_hash<> T_p2;
    det_hash<> T_p1;
    combine_hashes(dets_C_hash, T_p1);
    det_hash<> Ck;
    combine_hashes(T_p1, Ck);
    scale(Ck,boost::math::cyl_bessel_i(0, range_));
    combine_hashes(Ck, spawned);
    Ck.clear();

    det_hash<> Tk;
    det_vec sub_dets;
    std::vector<double> sub_C;
    copy_hash_to_vec(T_p1,sub_dets,sub_C);
    apply_tau_H(-tau/range_,spawning_threshold,sub_dets,sub_C,Tk,S);
//    det_hash<> C1;
    combine_hashes(Tk, Ck);
    scale(Ck, 2.0 * boost::math::cyl_bessel_i(1, range_));
    combine_hashes(Ck, spawned);
    Ck.clear();

    for (int i = 2; i<= chebyshev_order_; i++){
        Ck.clear();
        T_p2 = T_p1;
        T_p1 = Tk;
        Tk.clear();
        det_hash<> HT_p1;
        copy_hash_to_vec(T_p1, sub_dets,sub_C);
        apply_tau_H(-tau/range_,spawning_threshold,sub_dets,sub_C,HT_p1,S);
        scale(HT_p1, 2.0);
        combine_hashes(HT_p1, Tk);
        add(Tk, -1.0, T_p2);
        combine_hashes(Tk, Ck);
        scale(Ck, 2.0 * boost::math::cyl_bessel_i(i, range_));
        combine_hashes(Ck, spawned);
    }
    normalize(spawned);

    copy_hash_to_vec(spawned,dets,C);
}

void AdaptivePathIntegralCI::propagate_Trotter_linear(det_vec& dets,std::vector<double>& C,double tau,double spawning_threshold,double S)
{
    // A map that contains the pair (determinant,coefficient)
    det_hash<> dets_C_hash;

    // Term 1. |n>
    for (size_t I = 0, max_I = dets.size(); I < max_I; ++I){
        dets_C_hash[dets[I]] = C[I];
    }
    // Term 2. -tau (H - S)|n>
    apply_tau_H(-tau,spawning_threshold,dets,C,dets_C_hash,S);

    // Correct the diagonals
    for (size_t I = 0, max_I = dets.size(); I < max_I; ++I){
        double det_energy = dets[I].energy();
        double CI = dets_C_hash[dets[I]];
        dets_C_hash[dets[I]] += tau * (det_energy - S) * CI;
        dets_C_hash[dets[I]] += exp(-tau * (det_energy - S)) * CI;

    }

    // Overwrite the input vectors with the updated wave function
    copy_hash_to_vec(dets_C_hash,dets,C);
}

void AdaptivePathIntegralCI::propagate_Olsen(det_vec& dets,std::vector<double>& C,double spawning_threshold,double S)
{
    // A map that contains the pair (determinant,coefficient)
    det_hash<> dets_C_hash;

    // 1.  Compute H - E (S = E)
    apply_tau_H(1.0,spawning_threshold,dets,C,dets_C_hash,S);

    double delta_E_num = 0.0;
    double delta_E_den = 0.0;
    for (size_t I = 0, max_I = dets.size(); I < max_I; ++I){
        double CI = C[I];
        double EI = dets[I].energy();
        double sigma_I = dets_C_hash[dets[I]];
        delta_E_num += CI * sigma_I / (EI - S);
        delta_E_den += CI * CI / (EI - S);
    }
    double delta_E = delta_E_num / delta_E_den;

    for (size_t I = 0, max_I = dets.size(); I < max_I; ++I){
        dets_C_hash[dets[I]] -= C[I] * delta_E;
    }

    double step_norm = 0.0;
    for (auto& det_C : dets_C_hash){
        double EI = det_C.first.energy();
        det_C.second /= - (EI - S);
        step_norm += det_C.second * det_C.second;
    }
    step_norm = std::sqrt(step_norm);


    double max_norm = 0.05;
    if (step_norm > max_norm){
        outfile->Printf("\n\t  Step norm = %f is greather than %f.  Rescaling Olsen step.",step_norm,max_norm);
        double factor = max_norm / step_norm;
        for (auto& det_C : dets_C_hash){
            det_C.second *= factor;
        }
    }

    double sum = 0.0;
    for (size_t I = 0, max_I = dets.size(); I < max_I; ++I){
        sum += std::fabs(dets_C_hash[dets[I]]);
        dets_C_hash[dets[I]] += C[I];
    }

    double norm = 0.0;
    for (auto& det_C : dets_C_hash){
        norm += std::pow(det_C.second,2.0);
    }
    norm = std::sqrt(norm);
    for (auto& det_C : dets_C_hash){
        det_C.second /= norm;
    }

    // Overwrite the input vectors with the updated wave function
    copy_hash_to_vec(dets_C_hash,dets,C);
}

void AdaptivePathIntegralCI::propagate_DavidsonLiu(det_vec& dets,std::vector<double>& C,double spawning_threshold)
{
    throw PSIEXCEPTION("\n\n  propagate_DavidsonLiu is not implemented yet.\n\n");

    det_hash<> dets_C_hash;

    int maxiter = 50;
    bool print = false;

    // Number of roots
    int M = 1;

    size_t collapse_size = 1 * M;
    size_t subspace_size = 8 * M;

    double e_convergence = 1.0e-10;

    // current set of guess vectors
    std::vector<det_hash<>> b(subspace_size);

    // guess vectors formed from old vectors, stored by row
    std::vector<det_hash<>> bnew(subspace_size);

    // residual eigenvectors, stored by row
    std::vector<det_hash<>> r(subspace_size);

    // sigma vectors, stored by column
    std::vector<det_hash<>> sigma(subspace_size);

    // Davidson mini-Hamitonian
    Matrix G("G",subspace_size, subspace_size);
    // A metric matrix
    Matrix S("S",subspace_size, subspace_size);
    // Eigenvectors of the Davidson mini-Hamitonian
    Matrix alpha("alpha",subspace_size, subspace_size);
    Matrix alpha_t("alpha",subspace_size, subspace_size);
    // Eigenvalues of the Davidson mini-Hamitonian
    Vector lambda("lambda",subspace_size);
    double* lambda_p = lambda.pointer();
    // Old eigenvalues of the Davidson mini-Hamitonian
    Vector lambda_old("lambda",subspace_size);

    // Set b[0]
    for (size_t I = 0, max_I = C.size(); I < max_I; ++I){
        b[0][dets[I]] = C[I];
    }

    size_t L = 1;
    int iter = 0;
    int converged = 0;
    double old_energy = 0.0;
    while((converged < M) and (iter < maxiter)) {
        bool skip_check = false;
        if(print) outfile->Printf("\n  iter = %d\n", iter);

        // Step #2: Build and Diagonalize the Subspace Hamiltonian
        for (size_t l = 0; l < L; ++l){
            sigma[l].clear();
//            apply_tau_H(1.0,spawning_threshold,b[l],sigma[l],0.0); <= TODO : re-enable
        }

        G.zero();
        S.zero();
        for (size_t i = 0; i < L; ++i){
            for (size_t j = 0; j < L; ++j){
                double g = 0.0;
                auto& sigma_j = sigma[j];
                for (auto& det_b_i : b[i]){
                    g += det_b_i.second * sigma_j[det_b_i.first];
                }
                G.set(i,j,g);

                double s = 0.0;
                auto& b_j = b[j];
                for (auto& det_b_i : b[i]){
                    s += det_b_i.second * b_j[det_b_i.first];
                }
                S.set(i,j,s);
            }
        }

        S.power(-0.5);
        G.transform(S);
        G.diagonalize(alpha,lambda);
        alpha_t.gemm(false,false,1.0,S,alpha,0.0);
        double** alpha_p = alpha_t.pointer();

        dets_C_hash.clear();
        for(int i = 0; i < L; i++) {
            for (auto& det_b_i : b[i]){
                dets_C_hash[det_b_i.first] += alpha_p[i][0] * det_b_i.second;
            }
        }

        copy_hash_to_vec(dets_C_hash,dets,C);
        double var_energy = estimate_var_energy_sparse(dets,C,1.0e-8);

        double var_energy_gradient = var_energy - old_energy;
        old_energy = var_energy;
        outfile->Printf("\n%9d %8.4f %10zu %20.12f %.3e %20.12f %.3e",iter,0.0,C.size(),
                        0.0,0.0,
                        var_energy,var_energy_gradient);

        // If L is close to maxdim, collapse to one guess per root */
        if(subspace_size - L < M) {
            if(print) {
                outfile->Printf("Subspace too large: maxdim = %d, L = %d\n", subspace_size, L);
                outfile->Printf("Collapsing eigenvectors.\n");
            }
            for(int k = 0; k < collapse_size; k++){
                bnew[k].clear();
                auto& bnew_k = bnew[k];
                for(int i = 0; i < L; i++) {
                    for (auto& det_b_i : b[i]){
                        bnew_k[det_b_i.first] += alpha_p[i][k] * det_b_i.second;
                    }
                }
            }

            // Copy them into place
            L = 0;
            for(int k = 0; k < collapse_size; k++){
                b[k].clear();
                auto& b_k = b[k];
                for (auto& det_bnew_k : bnew[k]){
                    b_k[det_bnew_k.first] = det_bnew_k.second;
                }
                L++;
            }

            skip_check = true;

            // Step #2: Build and Diagonalize the Subspace Hamiltonian
            for (size_t l = 0; l < L; ++l){
                sigma[l].clear();
//                apply_tau_H(1.0,spawning_threshold,b[l],sigma[l],0.0); <= TODO : re-enable
            }

            // Rebuild and Diagonalize the Subspace Hamiltonian
            G.zero();
            S.zero();
            for (size_t i = 0; i < L; ++i){
                for (size_t j = 0; j < L; ++j){
                    double g = 0.0;
                    auto& sigma_j = sigma[j];
                    for (auto& det_b_i : b[i]){
                        g += det_b_i.second * sigma_j[det_b_i.first];
                    }
                    G.set(i,j,g);

                    double s = 0.0;
                    auto& b_j = b[j];
                    for (auto& det_b_i : b[i]){
                        s += det_b_i.second * b_j[det_b_i.first];
                    }
                    S.set(i,j,s);
                }
            }
            for (size_t i = 1; i < L; ++i){
                for (size_t j = 1; j < L; ++j){
                    if (i != j){
                        G.set(i,j,0.0);
                    }
                }
            }

            S.power(-0.5);
            G.transform(S);
            G.diagonalize(alpha,lambda);
            alpha_t.gemm(false,false,1.0,S,alpha,0.0);
        }

        // Step #3: Build the Correction Vectors
        // form preconditioned residue vectors
        for(int k = 0; k < M; k++){  // loop over roots
            r[k].clear();
            auto& r_k = r[k];
            for(int i = 0; i < L; i++) {
                for (auto& det_sigma_i : sigma[i]){
                    r_k[det_sigma_i.first] += alpha_p[i][k] * det_sigma_i.second;
                }
            }
            for(int i = 0; i < L; i++) {
                for (auto& det_b_i : b[i]){
                    r_k[det_b_i.first] -= alpha_p[i][k] * lambda_p[k] * det_b_i.second;
                }
            }

            for (auto& det_r_k : r_k){
                double denom = lambda_p[k] - det_r_k.first.energy();
                if(fabs(denom) > 1e-6){
                    det_r_k.second  /= denom;
                }
                else{
                    det_r_k.second  = 0.0;
                }
            }
        }

        // Step #4: Add the new correction vectors
        for(int k = 0; k < M; k++){  // loop over roots
            auto& r_k = r[k];
            auto& b_new = b[L];
            for (auto& det_r_k : r_k){
                b_new[det_r_k.first] = det_r_k.second;
            }
            // Orthogonalize to previous roots
            for (int i = 0; i < L; ++i){
                double s_i = 0.0;
                double m_i = 0.0;
                auto& b_i = b[i];
                for (auto& det_b_new : b_new){
                    s_i += det_b_new.second * b_i[det_b_new.first];
                }
                for (auto& det_b_i : b_i){
                    m_i += det_b_i.second * det_b_i.second;
                }
                for (auto& det_b_i : b_i){
                    b_new[det_b_i.first] -= s_i * det_b_i.second / m_i;
                }
            }
            L++;
        }


//        /* normalize each residual */
//        for(int k = 0; k < M; k++) {
//            double norm = 0.0;
//            for(int I = 0; I < N; I++) {
//                norm += f_p[k][I] * f_p[k][I];
//            }
//            norm = std::sqrt(norm);
//            for(int I = 0; I < N; I++) {
//                f_p[k][I] /= norm;
//            }
//        }

//        // schmidt orthogonalize the f[k] against the set of b[i] and add new vectors
//        for(int k = 0; k < M; k++){
//            if (L < subspace_size){
//                if(schmidt_add(b_p, L, N, f_p[k])) {
//                    L++;  // <- Increase L if we add one more basis vector
//                }
//            }
//        }

        // check convergence on all roots
        if(!skip_check) {
            converged = 0;
            if(print) {
                outfile->Printf("Root      Eigenvalue       Delta  Converged?\n");
                outfile->Printf("---- -------------------- ------- ----------\n");
            }
            for(int k = 0; k < M; k++) {
                double diff = std::fabs(lambda.get(k) - lambda_old.get(k));
                bool this_converged = false;
                if(diff < e_convergence) {
                    this_converged = true;
                    converged++;
                }
                lambda_old.set(k,lambda.get(k));
                if(print) {
                    outfile->Printf("%3d  %20.14f %4.3e    %1s\n", k, lambda.get(k) + nuclear_repulsion_energy_, diff,
                           this_converged ? "Y" : "N");
                }
            }
        }

        outfile->Flush();

        iter++;
    }

//    /* generate final eigenvalues and eigenvectors */
//    //if(converged == M) {
//    double** alpha_p = alpha.pointer();
//    double** b_p = b.pointer();
//    for(int i = 0; i < M; i++) {
//        eps[i] = lambda.get(i);
//        for(int I = 0; I < N; I++){
//            v[I][i] = 0.0;
//        }
//        for(int j = 0; j < L; j++) {
//            for(int I=0; I < N; I++) {
//                v[I][i] += alpha_p[j][i] * b_p[j][I];
//            }
//        }
//        // Normalize v
//        double norm = 0.0;
//        for(int I = 0; I < N; I++) {
//            norm += v[I][i] * v[I][i];
//        }
//        norm = std::sqrt(norm);
//        for(int I = 0; I < N; I++) {
//            v[I][i] /= norm;
//        }
//    }

    copy_hash_to_vec(dets_C_hash,dets,C);



    outfile->Printf("\n  The Davidson-Liu algorithm converged in %d iterations.", iter);
    double var_energy = estimate_var_energy_sparse(dets,C,1.0e-14);
    outfile->Printf("\n  * Adaptive-CI Variational Energy     = %.12f Eh",var_energy);
//    outfile->Printf("\n  %s: %f s","Time spent diagonalizing H",t_davidson.elapsed());
}

void AdaptivePathIntegralCI::apply_tau_H(double tau,double spawning_threshold,det_vec& dets,const std::vector<double>& C, det_hash<>& dets_C_hash, double S)
{
    // A vector of maps that hold (determinant,coefficient)
//    std::vector<det_hash<>> thread_det_C_hash(num_threads_);
    std::vector<std::pair<double,double>> thread_max_HJI(num_threads_);

    if(do_dynamic_prescreening_){
        size_t max_I = dets.size();
#pragma omp parallel for
        for (size_t I = 0; I < max_I; ++I){
            std::pair<double,double> zero_pair(0.0,0.0);
            // Update the list of couplings
            std::pair<double,double> max_coupling;
            #pragma omp critical
            {
                max_coupling = dets_max_couplings_[dets[I]];
            }
            if (max_coupling == zero_pair){
                std::vector<std::pair<Determinant, double>> thread_det_C_vec;
                apply_tau_H_det_dynamic(tau,spawning_threshold,dets[I],C[I],thread_det_C_vec,S,max_coupling);
                #pragma omp critical
                {
                    for (auto det_C : thread_det_C_vec) {
                        dets_C_hash[det_C.first] += det_C.second;
                    }
                }
                #pragma omp critical
                {
                    dets_max_couplings_[dets[I]] = max_coupling;
                }
            }else{
                std::vector<std::pair<Determinant, double>> thread_det_C_vec;
                apply_tau_H_det_dynamic(tau,spawning_threshold,dets[I],C[I],thread_det_C_vec,S,max_coupling);
                #pragma omp critical
                {
                    for (auto det_C : thread_det_C_vec) {
                        dets_C_hash[det_C.first] += det_C.second;
                    }
                }
            }
        }
        // Combine the results of all the threads
    }else if (do_schwarz_prescreening_){
        if (do_initiator_approx_) {
            size_t max_I = dets.size();
#pragma omp parallel for
            for (size_t I = 0; I < max_I; ++I){
                if (fabs(C[I]) >= initiator_approx_factor_*spawning_threshold) {
                    std::vector<std::pair<Determinant, double>> thread_det_C_vec;
                    apply_tau_H_det_schwarz(tau,spawning_threshold,dets[I],C[I],thread_det_C_vec,S);
                    #pragma omp critical
                    {
                        for (auto det_C : thread_det_C_vec) {
                            dets_C_hash[det_C.first] += det_C.second;
                        }
                    }
                } else {
                    // Diagonal contribution
                    double det_energy = dets[I].energy();
                    // Diagonal contributions
                    #pragma omp critical
                    {
                        dets_C_hash[dets[I]] += tau * (det_energy - S) * C[I];
                    }
                }
            }
        } else {
            size_t max_I = dets.size();
#pragma omp parallel for
            for (size_t I = 0; I < max_I; ++I){
                std::vector<std::pair<Determinant, double>> thread_det_C_vec;
                apply_tau_H_det_schwarz(tau,spawning_threshold,dets[I],C[I],thread_det_C_vec,S);
                #pragma omp critical
                {
                    for (auto det_C : thread_det_C_vec) {
                        dets_C_hash[det_C.first] += det_C.second;
                    }
                }
            }
        }

        // Combine the results of all the threads
    }else if (do_initiator_approx_){
        size_t max_I = dets.size();
#pragma omp parallel for
        for (size_t I = 0; I < max_I; ++I){
            int thread_id = omp_get_thread_num();
            if (fabs(C[I]) >= initiator_approx_factor_*spawning_threshold) {
                std::vector<std::pair<Determinant, double>> thread_det_C_vec;
                const std::pair<double,double> max_HJI = apply_tau_H_det_prescreening(tau,spawning_threshold,dets[I],C[I],thread_det_C_vec,S);
                #pragma omp critical
                {
                    for (auto det_C : thread_det_C_vec) {
                        dets_C_hash[det_C.first] += det_C.second;
                    }
                }
                thread_max_HJI[thread_id].first = std::max(thread_max_HJI[thread_id].first,max_HJI.first);    // to avoid race condition
                thread_max_HJI[thread_id].second = std::max(thread_max_HJI[thread_id].second,max_HJI.second); // to avoid race condition
            } else {
                double det_energy = dets[I].energy();
                // Diagonal contributions
                #pragma omp critical
                {
                    dets_C_hash[dets[I]] += tau * (det_energy - S) * C[I];
                }
            }
        }
        // Combine the bounds on the couplings
        for (int t = 0; t < num_threads_; ++t){
            new_max_one_HJI_ = std::max(thread_max_HJI[t].first,new_max_one_HJI_);
            new_max_two_HJI_ = std::max(thread_max_HJI[t].second,new_max_two_HJI_);
        }
    }
    else{
        size_t max_I = dets.size();
#pragma omp parallel for
        for (size_t I = 0; I < max_I; ++I){
            int thread_id = omp_get_thread_num();
            std::vector<std::pair<Determinant, double>> thread_det_C_vec;
            const std::pair<double,double> max_HJI = apply_tau_H_det_prescreening(tau,spawning_threshold,dets[I],C[I],thread_det_C_vec,S);
            #pragma omp critical
            {
                for (auto det_C : thread_det_C_vec) {
                    dets_C_hash[det_C.first] += det_C.second;
                }
            }
            thread_max_HJI[thread_id].first = std::max(thread_max_HJI[thread_id].first,max_HJI.first);    // to avoid race condition
            thread_max_HJI[thread_id].second = std::max(thread_max_HJI[thread_id].second,max_HJI.second); // to avoid race condition
        }
        // Combine the bounds on the couplings
        for (int t = 0; t < num_threads_; ++t){
            new_max_one_HJI_ = std::max(thread_max_HJI[t].first,new_max_one_HJI_);
            new_max_two_HJI_ = std::max(thread_max_HJI[t].second,new_max_two_HJI_);
        }
    }

}

std::pair<double,double> AdaptivePathIntegralCI::apply_tau_H_det_prescreening(double tau, double spawning_threshold, Determinant &detI, double CI, std::vector<std::pair<Determinant, double>>& new_space_C_vec, double E0)
{
    bool do_singles = std::fabs(prescreening_tollerance_factor_ * old_max_one_HJI_ * CI) >= spawning_threshold;
    bool do_doubles = std::fabs(prescreening_tollerance_factor_ * old_max_two_HJI_ * CI) >= spawning_threshold;

    // Diagonal contributions
    double det_energy = detI.energy();
//    new_space_C[detI] += tau * (det_energy - E0) * CI;
    new_space_C_vec.push_back(std::make_pair(detI, tau * (det_energy - E0) * CI));

    if (do_singles or do_doubles){
        std::vector<int> aocc = detI.get_alfa_occ();
        std::vector<int> bocc = detI.get_beta_occ();
        std::vector<int> avir = detI.get_alfa_vir();
        std::vector<int> bvir = detI.get_beta_vir();
        std::vector<int> aocc_offset(nirrep_ + 1);
        std::vector<int> bocc_offset(nirrep_ + 1);
        std::vector<int> avir_offset(nirrep_ + 1);
        std::vector<int> bvir_offset(nirrep_ + 1);

        int noalpha = aocc.size();
        int nobeta  = bocc.size();
        int nvalpha = avir.size();
        int nvbeta  = bvir.size();

        for (int i = 0; i < noalpha; ++i) aocc_offset[mo_symmetry_[aocc[i]] + 1] += 1;
        for (int a = 0; a < nvalpha; ++a) avir_offset[mo_symmetry_[avir[a]] + 1] += 1;
        for (int i = 0; i < nobeta; ++i) bocc_offset[mo_symmetry_[bocc[i]] + 1] += 1;
        for (int a = 0; a < nvbeta; ++a) bvir_offset[mo_symmetry_[bvir[a]] + 1] += 1;
        for (int h = 1; h < nirrep_ + 1; ++h){
            aocc_offset[h] += aocc_offset[h-1];
            avir_offset[h] += avir_offset[h-1];
            bocc_offset[h] += bocc_offset[h-1];
            bvir_offset[h] += bvir_offset[h-1];
        }

        double my_new_max_one_HJI = 0.0;
        double my_new_max_two_HJI = 0.0;

        if (do_singles){
            Determinant detJ(detI);
#if ENFORCE_SYM
            // Generate aa excitations
            for (int h = 0; h < nirrep_; ++h){
                for (int i = aocc_offset[h]; i < aocc_offset[h + 1]; ++i){
                    int ii = aocc[i];
                    for (int a = avir_offset[h]; a < avir_offset[h + 1]; ++a){
                        int aa = avir[a];
                        double HJI = detI.slater_rules_single_alpha(ii,aa);
                        my_new_max_one_HJI = std::max(my_new_max_one_HJI,std::fabs(HJI));
                        if (std::fabs(HJI * CI) >= spawning_threshold){
                            detJ = detI;
                            detJ.set_alfa_bit(ii,false);
                            detJ.set_alfa_bit(aa,true);
//                            new_space_C[detJ] += tau * HJI * CI;
                            new_space_C_vec.push_back(std::make_pair(detJ, tau * HJI * CI));
                        }
                    }
                }
            }
            // Generate bb excitations
            for (int h = 0; h < nirrep_; ++h){
                for (int i = bocc_offset[h]; i < bocc_offset[h + 1]; ++i){
                    int ii = bocc[i];
                    for (int a = bvir_offset[h]; a < bvir_offset[h + 1]; ++a){
                        int aa = bvir[a];
                        double HJI = detI.slater_rules_single_beta(ii,aa);
                        my_new_max_one_HJI = std::max(my_new_max_one_HJI,std::fabs(HJI));
                        if (std::fabs(HJI * CI) >= spawning_threshold){
                            detJ = detI;
                            detJ.set_beta_bit(ii,false);
                            detJ.set_beta_bit(aa,true);
//                            new_space_C[detJ] += tau * HJI * CI;
                            new_space_C_vec.push_back(std::make_pair(detJ, tau * HJI * CI));
                        }
                    }
                }
            }
        }

        if (do_doubles){
            Determinant detJ(detI);
            for (int i = 0; i < noalpha; ++i){
                int ii = aocc[i];
                for (int j = i + 1; j < noalpha; ++j){
                    int jj = aocc[j];
                    for (int a = 0; a < nvalpha; ++a){
                        int aa = avir[a];
                        int h = mo_symmetry_[ii] ^ mo_symmetry_[jj] ^ mo_symmetry_[aa];
                        if (h < mo_symmetry_[aa]) continue;
                        int minb = h == mo_symmetry_[aa] ? a + 1 : avir_offset[h];
                        int maxb = avir_offset[h + 1];
                        for (int b = minb; b < maxb; ++b){
                            int bb = avir[b];
                            double HJI = fci_ints_->tei_aa(ii,jj,aa,bb);
                            my_new_max_two_HJI = std::max(my_new_max_two_HJI,std::fabs(HJI));
                            if (std::fabs(HJI * CI) >= spawning_threshold){
                                detJ = detI;
                                HJI *= detJ.double_excitation_aa(ii,jj,aa,bb);
//                                new_space_C[detJ] += tau * HJI * CI;
                                new_space_C_vec.push_back(std::make_pair(detJ, tau * HJI * CI));
                            }
                        }
                    }
                }
            }

            for (int i = 0; i < noalpha; ++i){
                int ii = aocc[i];
                for (int j = 0; j < nobeta; ++j){
                    int jj = bocc[j];
                    for (int a = 0; a < nvalpha; ++a){
                        int aa = avir[a];
                        int h = mo_symmetry_[ii] ^ mo_symmetry_[jj] ^ mo_symmetry_[aa];
                        int minb = bvir_offset[h];
                        int maxb = bvir_offset[h + 1];
                        for (int b = minb; b < maxb; ++b){
                            int bb = bvir[b];
                            double HJI = fci_ints_->tei_ab(ii,jj,aa,bb);
                            my_new_max_two_HJI = std::max(my_new_max_two_HJI,std::fabs(HJI));

                            if (std::fabs(HJI * CI) >= spawning_threshold){
                                detJ = detI;
                                HJI *= detJ.double_excitation_ab(ii,jj,aa,bb);
//                                new_space_C[detJ] += tau * HJI * CI;
                                new_space_C_vec.push_back(std::make_pair(detJ, tau * HJI * CI));
                            }
                        }
                    }
                }
            }
            for (int i = 0; i < nobeta; ++i){
                int ii = bocc[i];
                for (int j = i + 1; j < nobeta; ++j){
                    int jj = bocc[j];
                    for (int a = 0; a < nvbeta; ++a){
                        int aa = bvir[a];
                        int h = mo_symmetry_[ii] ^ mo_symmetry_[jj] ^ mo_symmetry_[aa];
                        if (h < mo_symmetry_[aa]) continue;
                        int minb = h == mo_symmetry_[aa] ? a + 1 : bvir_offset[h];
                        int maxb = bvir_offset[h + 1];
                        for (int b = minb; b < maxb; ++b){
                            int bb = bvir[b];
                            double HJI = fci_ints_->tei_bb(ii,jj,aa,bb);
                            my_new_max_two_HJI = std::max(my_new_max_two_HJI,std::fabs(HJI));

                            if (std::fabs(HJI * CI) >= spawning_threshold){
                                detJ = detI;
                                HJI *= detJ.double_excitation_bb(ii,jj,aa,bb);
//                                new_space_C[detJ] += tau * HJI * CI;
                                new_space_C_vec.push_back(std::make_pair(detJ, tau * HJI * CI));
                            }
                        }
                    }
                }
            }
        }
#else
            for (int i = 0; i < noalpha; ++i){
                int ii = aocc[i];
                for (int a = 0; a < nvalpha; ++a){
                    int aa = avir[a];
                    if ((mo_symmetry_[ii] ^ mo_symmetry_[aa]) == 0){
                        double HJI = detI.slater_rules_single_alpha(ii,aa);
                        my_new_max_one_HJI = std::max(my_new_max_one_HJI,std::fabs(HJI));
                        if (std::fabs(HJI * CI) >= spawning_threshold){
                            detJ = detI;
                            detJ.set_alfa_bit(ii,false);
                            detJ.set_alfa_bit(aa,true);
                            new_space_C[detJ] += tau * HJI * CI;
                        }
                    }
                }
            }

            for (int i = 0; i < nobeta; ++i){
                int ii = bocc[i];
                for (int a = 0; a < nvbeta; ++a){
                    int aa = bvir[a];
                    if ((mo_symmetry_[ii] ^ mo_symmetry_[aa])  == 0){
                        double HJI = detI.slater_rules_single_beta(ii,aa);
                        my_new_max_one_HJI = std::max(my_new_max_one_HJI,std::fabs(HJI));
                        if (std::fabs(HJI * CI) >= spawning_threshold){
                            detJ = detI;
                            detJ.set_beta_bit(ii,false);
                            detJ.set_beta_bit(aa,true);
                            new_space_C[detJ] += tau * HJI * CI;
                        }
                    }
                }
            }
        }

        if (do_doubles){
            Determinant detJ(detI);
            for (int i = 0; i < noalpha; ++i){
                int ii = aocc[i];
                for (int j = i + 1; j < noalpha; ++j){
                    int jj = aocc[j];
                    for (int a = 0; a < nvalpha; ++a){
                        int aa = avir[a];
                        for (int b = a + 1; b < nvalpha; ++b){
                            int bb = avir[b];
                            if ((mo_symmetry_[ii] ^ mo_symmetry_[jj] ^ mo_symmetry_[aa] ^ mo_symmetry_[bb]) == 0){
                                double HJI = fci_ints_->tei_aa(ii,jj,aa,bb);
                                my_new_max_two_HJI = std::max(my_new_max_two_HJI,std::fabs(HJI));
                                if (std::fabs(HJI * CI) >= spawning_threshold){
                                    detJ = detI;
                                    detJ.set_alfa_bit(ii,false);
                                    detJ.set_alfa_bit(jj,false);
                                    detJ.set_alfa_bit(aa,true);
                                    detJ.set_alfa_bit(bb,true);

                                    // grap the alpha bits of both determinants
                                    const Determinant::bit_t& Ia = detI.alfa_bits();
                                    const Determinant::bit_t& Ja = detJ.alfa_bits();

                                    // compute the sign of the matrix element
                                    HJI *= Determinant::SlaterSign(Ia,ii) * Determinant::SlaterSign(Ia,jj) * Determinant::SlaterSign(Ja,aa) * Determinant::SlaterSign(Ja,bb);
                                    new_space_C[detJ] += tau * HJI * CI;
                                }
                            }
                        }
                    }
                }
            }

            for (int i = 0; i < noalpha; ++i){
                int ii = aocc[i];
                for (int j = 0; j < nobeta; ++j){
                    int jj = bocc[j];
                    for (int a = 0; a < nvalpha; ++a){
                        int aa = avir[a];
                        for (int b = 0; b < nvbeta; ++b){
                            int bb = bvir[b];
                            if ((mo_symmetry_[ii] ^ mo_symmetry_[jj] ^ mo_symmetry_[aa] ^ mo_symmetry_[bb]) == 0){
                                double HJI = fci_ints_->tei_ab(ii,jj,aa,bb);
                                my_new_max_two_HJI = std::max(my_new_max_two_HJI,std::fabs(HJI));

                                if (std::fabs(HJI * CI) >= spawning_threshold){
                                    detJ = detI;
                                    detJ.set_alfa_bit(ii,false);
                                    detJ.set_beta_bit(jj,false);
                                    detJ.set_alfa_bit(aa,true);
                                    detJ.set_beta_bit(bb,true);

                                    // grap the alpha bits of both determinants
                                    const Determinant::bit_t& Ia = detI.alfa_bits();
                                    const Determinant::bit_t& Ib = detI.beta_bits();
                                    const Determinant::bit_t& Ja = detJ.alfa_bits();
                                    const Determinant::bit_t& Jb = detJ.beta_bits();

                                    // compute the sign of the matrix element
                                    HJI *= Determinant::SlaterSign(Ia,ii) * Determinant::SlaterSign(Ib,jj) * Determinant::SlaterSign(Ja,aa) * Determinant::SlaterSign(Jb,bb);
                                    new_space_C[detJ] += tau * HJI * CI;
                                }
                            }
                        }
                    }
                }
            }
            for (int i = 0; i < nobeta; ++i){
                int ii = bocc[i];
                for (int j = i + 1; j < nobeta; ++j){
                    int jj = bocc[j];
                    for (int a = 0; a < nvbeta; ++a){
                        int aa = bvir[a];
                        for (int b = a + 1; b < nvbeta; ++b){
                            int bb = bvir[b];
                            if ((mo_symmetry_[ii] ^ (mo_symmetry_[jj] ^ (mo_symmetry_[aa] ^ mo_symmetry_[bb]))) == 0){
                                double HJI = fci_ints_->tei_bb(ii,jj,aa,bb);
                                my_new_max_two_HJI = std::max(my_new_max_two_HJI,std::fabs(HJI));

                                if (std::fabs(HJI * CI) >= spawning_threshold){
                                    detJ = detI;
                                    detJ.set_beta_bit(ii,false);
                                    detJ.set_beta_bit(jj,false);
                                    detJ.set_beta_bit(aa,true);
                                    detJ.set_beta_bit(bb,true);

                                    // grap the alpha bits of both determinants
                                    const Determinant::bit_t& Ib = detI.beta_bits();
                                    const Determinant::bit_t& Jb = detJ.beta_bits();

                                    // compute the sign of the matrix element
                                    HJI *= Determinant::SlaterSign(Ib,ii) * Determinant::SlaterSign(Ib,jj) * Determinant::SlaterSign(Jb,aa) * Determinant::SlaterSign(Jb,bb);
                                    new_space_C[detJ] += tau * HJI * CI;
                                }
                            }
                        }
                    }
                }
            }
        }
#endif
        return std::make_pair(my_new_max_one_HJI,my_new_max_two_HJI);
    }
    return std::make_pair(0.0,0.0);
}

void AdaptivePathIntegralCI::apply_tau_H_det_schwarz(double tau, double spawning_threshold, const Determinant &detI, double CI, std::vector<std::pair<Determinant, double> > &new_space_C_vec, double E0)
{
    std::vector<int> aocc = detI.get_alfa_occ();
    std::vector<int> bocc = detI.get_beta_occ();
    std::vector<int> avir = detI.get_alfa_vir();
    std::vector<int> bvir = detI.get_beta_vir();

    int noalpha = aocc.size();
    int nobeta  = bocc.size();
    int nvalpha = avir.size();
    int nvbeta  = bvir.size();

    // Diagonal contributions
    double det_energy = detI.energy();
    new_space_C_vec.push_back(std::make_pair(detI, tau * (det_energy - E0) * CI));

    // Generate aa excitations
    for (int i = 0; i < noalpha; ++i){
        int ii = aocc[i];
        for (int a = 0; a < nvalpha; ++a){
            int aa = avir[a];
            if ((mo_symmetry_[ii] ^ mo_symmetry_[aa]) == 0){
                Determinant detJ(detI);
                detJ.set_alfa_bit(ii,false);
                detJ.set_alfa_bit(aa,true);
                double HJI = detJ.slater_rules(detI);
                if (std::fabs(HJI * CI) >= spawning_threshold){
                    new_space_C_vec.push_back(std::make_pair(detJ, tau * HJI * CI));
                }
            }
        }
    }

    for (int i = 0; i < nobeta; ++i){
        int ii = bocc[i];
        for (int a = 0; a < nvbeta; ++a){
            int aa = bvir[a];
            if ((mo_symmetry_[ii] ^ mo_symmetry_[aa])  == 0){
                Determinant detJ(detI);
                detJ.set_beta_bit(ii,false);
                detJ.set_beta_bit(aa,true);
                double HJI = detJ.slater_rules(detI);
                if (std::fabs(HJI * CI) >= spawning_threshold){
                    new_space_C_vec.push_back(std::make_pair(detJ, tau * HJI * CI));
                }
            }
        }
    }

    // Generate aa excitations
    double pqpq_max_ab = 0.0;
    for (int a = 0; a < nvalpha; ++a){
        int aa = avir[a];
        for (int b = a + 1; b < nvalpha; ++b){
            int bb = avir[b];
            pqpq_max_ab = std::max(pqpq_aa_[aa*ncmo_+bb],pqpq_max_ab);
        }
    }
    for (int i = 0; i < noalpha; ++i){
        int ii = aocc[i];
        for (int j = i + 1; j < noalpha; ++j){
            int jj = aocc[j];
            ++schwarz_total_;
            if (fabs(pqpq_aa_[ii * ncmo_ + jj] * pqpq_max_ab * CI) < spawning_threshold) {
                ++schwarz_succ_;
                continue;
            }
            for (int a = 0; a < nvalpha; ++a){
                int aa = avir[a];
                for (int b = a + 1; b < nvalpha; ++b){
                    int bb = avir[b];
                    if ((mo_symmetry_[ii] ^ mo_symmetry_[jj] ^ mo_symmetry_[aa] ^ mo_symmetry_[bb]) == 0){
                        double HJI = fci_ints_->tei_aa(ii,jj,aa,bb);

                        if (std::fabs(HJI * CI) >= spawning_threshold){
                            Determinant detJ(detI);
                            HJI *= detJ.double_excitation_aa(ii,jj,aa,bb);
                            new_space_C_vec.push_back(std::make_pair(detJ, tau * HJI * CI));
                        }
                    }
                }
            }
        }
    }

    pqpq_max_ab = 0.0;
    for (int a = 0; a < nvalpha; ++a){
        int aa = avir[a];
        for (int b = 0; b < nvbeta; ++b){
            int bb = bvir[b];
            pqpq_max_ab = std::max(pqpq_ab_[aa*ncmo_+bb],pqpq_max_ab);
        }
    }
    for (int i = 0; i < noalpha; ++i){
        int ii = aocc[i];
        for (int j = 0; j < nobeta; ++j){
            int jj = bocc[j];
            ++schwarz_total_;
            if (fabs(pqpq_ab_[ii * ncmo_ + jj] * pqpq_max_ab * CI) < spawning_threshold) {
                ++schwarz_succ_;
                continue;
            }
            for (int a = 0; a < nvalpha; ++a){
                int aa = avir[a];
                for (int b = 0; b < nvbeta; ++b){
                    int bb = bvir[b];
                    if ((mo_symmetry_[ii] ^ mo_symmetry_[jj] ^ mo_symmetry_[aa] ^ mo_symmetry_[bb]) == 0){
                        double HJI = fci_ints_->tei_ab(ii,jj,aa,bb);

                        if (std::fabs(HJI * CI) >= spawning_threshold){
                            Determinant detJ(detI);
                            HJI *= detJ.double_excitation_ab(ii,jj,aa,bb);
                            new_space_C_vec.push_back(std::make_pair(detJ, tau * HJI * CI));
                        }
                    }
                }
            }
        }
    }
    pqpq_max_ab = 0.0;
    for (int a = 0; a < nvalpha; ++a){
        int aa = bvir[a];
        for (int b = a + 1; b < nvbeta; ++b){
            int bb = bvir[b];
            pqpq_max_ab = std::max(pqpq_bb_[aa*ncmo_+bb],pqpq_max_ab);
        }
    }
    for (int i = 0; i < nobeta; ++i){
        int ii = bocc[i];
        for (int j = i + 1; j < nobeta; ++j){
            int jj = bocc[j];
            ++schwarz_total_;
            if (fabs(pqpq_bb_[ii * ncmo_ + jj] * pqpq_max_ab * CI) < spawning_threshold) {
                ++schwarz_succ_;
                continue;
            }
            for (int a = 0; a < nvbeta; ++a){
                int aa = bvir[a];
                for (int b = a + 1; b < nvbeta; ++b){
                    int bb = bvir[b];
                    if ((mo_symmetry_[ii] ^ (mo_symmetry_[jj] ^ (mo_symmetry_[aa] ^ mo_symmetry_[bb]))) == 0){
                        double HJI = fci_ints_->tei_bb(ii,jj,aa,bb);
                        if (std::fabs(HJI * CI) >= spawning_threshold){
                            Determinant detJ(detI);
                            HJI *= detJ.double_excitation_bb(ii,jj,aa,bb);
                            new_space_C_vec.push_back(std::make_pair(detJ, tau * HJI * CI));
                        }
                    }
                }
            }
        }
    }
}

void AdaptivePathIntegralCI::apply_tau_H_det_dynamic(double tau, double spawning_threshold, const Determinant &detI, double CI, std::vector<std::pair<Determinant, double> > &new_space_C_vec, double E0, std::pair<double,double>& max_coupling)
{
    bool do_singles = (max_coupling.first == 0.0) or (std::fabs(max_coupling.first * CI) >= spawning_threshold);
    bool do_doubles = (max_coupling.second == 0.0) or (std::fabs(max_coupling.second  * CI) >= spawning_threshold);

    // Diagonal contributions
    double det_energy = detI.energy();
    new_space_C_vec.push_back(std::make_pair(detI, tau * (det_energy - E0) * CI));

    if (do_singles or do_doubles){
        std::vector<int> aocc = detI.get_alfa_occ();
        std::vector<int> bocc = detI.get_beta_occ();
        std::vector<int> avir = detI.get_alfa_vir();
        std::vector<int> bvir = detI.get_beta_vir();

        int noalpha = aocc.size();
        int nobeta  = bocc.size();
        int nvalpha = avir.size();
        int nvbeta  = bvir.size();

        if (do_singles){
            // Generate alpha excitations
            for (int i = 0; i < noalpha; ++i){
                int ii = aocc[i];
                for (int a = 0; a < nvalpha; ++a){
                    int aa = avir[a];
                    if ((mo_symmetry_[ii] ^ mo_symmetry_[aa]) == 0){
                        Determinant detJ(detI);
                        detJ.set_alfa_bit(ii,false);
                        detJ.set_alfa_bit(aa,true);
                        double HJI = detJ.slater_rules(detI);
                        max_coupling.first = std::max(max_coupling.first,std::fabs(HJI));
                        if (std::fabs(HJI * CI) >= spawning_threshold){
                            new_space_C_vec.push_back(std::make_pair(detJ, tau * HJI * CI));
                        }
                    }
                }
            }
            // Generate beta excitations
            for (int i = 0; i < nobeta; ++i){
                int ii = bocc[i];
                for (int a = 0; a < nvbeta; ++a){
                    int aa = bvir[a];
                    if ((mo_symmetry_[ii] ^ mo_symmetry_[aa])  == 0){
                        Determinant detJ(detI);
                        detJ.set_beta_bit(ii,false);
                        detJ.set_beta_bit(aa,true);
                        double HJI = detJ.slater_rules(detI);
                        max_coupling.first = std::max(max_coupling.first,std::fabs(HJI));
                        if (std::fabs(HJI * CI) >= spawning_threshold){
                            new_space_C_vec.push_back(std::make_pair(detJ, tau * HJI * CI));
                        }
                    }
                }
            }
        }

        if (do_doubles){
            // Generate alpha-alpha excitations
            for (int i = 0; i < noalpha; ++i){
                int ii = aocc[i];
                for (int j = i + 1; j < noalpha; ++j){
                    int jj = aocc[j];
                    for (int a = 0; a < nvalpha; ++a){
                        int aa = avir[a];
                        for (int b = a + 1; b < nvalpha; ++b){
                            int bb = avir[b];
                            if ((mo_symmetry_[ii] ^ mo_symmetry_[jj] ^ mo_symmetry_[aa] ^ mo_symmetry_[bb]) == 0){
                                double HJI = fci_ints_->tei_aa(ii,jj,aa,bb);
                                max_coupling.second = std::max(max_coupling.second,std::fabs(HJI));
                                if (std::fabs(HJI * CI) >= spawning_threshold){
                                    Determinant detJ(detI);
                                    HJI *= detJ.double_excitation_aa(ii,jj,aa,bb);
                                    new_space_C_vec.push_back(std::make_pair(detJ, tau * HJI * CI));
                                }
                            }
                        }
                    }
                }
            }
            // Generate alpha-beta excitations
            for (int i = 0; i < noalpha; ++i){
                int ii = aocc[i];
                for (int j = 0; j < nobeta; ++j){
                    int jj = bocc[j];
                    for (int a = 0; a < nvalpha; ++a){
                        int aa = avir[a];
                        for (int b = 0; b < nvbeta; ++b){
                            int bb = bvir[b];
                            if ((mo_symmetry_[ii] ^ mo_symmetry_[jj] ^ mo_symmetry_[aa] ^ mo_symmetry_[bb]) == 0){
                                double HJI = fci_ints_->tei_ab(ii,jj,aa,bb);
                                max_coupling.second = std::max(max_coupling.second,std::fabs(HJI));
                                if (std::fabs(HJI * CI) >= spawning_threshold){
                                    Determinant detJ(detI);
                                    HJI *= detJ.double_excitation_ab(ii,jj,aa,bb);
                                    new_space_C_vec.push_back(std::make_pair(detJ, tau * HJI * CI));
                                }
                            }
                        }
                    }
                }
            }
            // Generate beta-beta excitations
            for (int i = 0; i < nobeta; ++i){
                int ii = bocc[i];
                for (int j = i + 1; j < nobeta; ++j){
                    int jj = bocc[j];
                    for (int a = 0; a < nvbeta; ++a){
                        int aa = bvir[a];
                        for (int b = a + 1; b < nvbeta; ++b){
                            int bb = bvir[b];
                            if ((mo_symmetry_[ii] ^ (mo_symmetry_[jj] ^ (mo_symmetry_[aa] ^ mo_symmetry_[bb]))) == 0){
                                double HJI = fci_ints_->tei_bb(ii,jj,aa,bb);
                                max_coupling.second = std::max(max_coupling.second,std::fabs(HJI));
                                if (std::fabs(HJI * CI) >= spawning_threshold){
                                    Determinant detJ(detI);
                                    HJI *= detJ.double_excitation_bb(ii,jj,aa,bb);
                                    new_space_C_vec.push_back(std::make_pair(detJ, tau * HJI * CI));
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

std::map<std::string,double> AdaptivePathIntegralCI::estimate_energy(det_vec& dets,std::vector<double>& C)
{
    std::map<std::string,double> results;

    timer_on("PIFCI:<E>p");
    results["PROJECTIVE ENERGY"] = estimate_proj_energy(dets,C);
    timer_off("PIFCI:<E>p");

    if (fast_variational_estimate_){
        timer_on("PIFCI:<E>vs");
        results["VARIATIONAL ENERGY"] = estimate_var_energy_sparse(dets,C,energy_estimate_threshold_);
        timer_off("PIFCI:<E>vs");
    }else{
        timer_on("PIFCI:<E>v");
        results["VARIATIONAL ENERGY"] = estimate_var_energy(dets,C,energy_estimate_threshold_);
        timer_off("PIFCI:<E>v");
    }

    return results;
}

static bool abs_compare(double a, double b)
{
    return (std::abs(a) < std::abs(b));
}

double AdaptivePathIntegralCI::estimate_proj_energy(det_vec& dets,std::vector<double>& C)
{
    // Find the determinant with the largest value of C
    auto result = std::max_element(C.begin(), C.end(), abs_compare);
    size_t J = std::distance(C.begin(), result);
    double CJ = C[J];

    // Compute the projective energy
    double projective_energy_estimator = 0.0;
    for (int I = 0, max_I = dets.size(); I < max_I; ++I){
        double HIJ = dets[I].slater_rules(dets[J]);
        projective_energy_estimator += HIJ * C[I] / CJ;
    }
    return projective_energy_estimator + nuclear_repulsion_energy_;
}


double AdaptivePathIntegralCI::estimate_var_energy(det_vec &dets, std::vector<double> &C,double tollerance)
{
    // Compute a variational estimator of the energy
    size_t size = dets.size();
    double variational_energy_estimator = 0.0;
#pragma omp parallel for reduction(+:variational_energy_estimator)
    for (size_t I = 0; I < size; ++I){
        const Determinant& detI = dets[I];
        variational_energy_estimator += C[I] * C[I] * detI.energy();
        for (size_t J = I + 1; J < size; ++J){
            if (std::fabs(C[I] * C[J]) > tollerance){
                double HIJ = dets[I].slater_rules(dets[J]);
                variational_energy_estimator += 2.0 * C[I] * HIJ * C[J];
            }
        }
    }
    return variational_energy_estimator + nuclear_repulsion_energy_;
}


double AdaptivePathIntegralCI::estimate_var_energy_sparse(det_vec &dets, std::vector<double> &C,double tollerance)
{
    // A map that contains the pair (determinant,coefficient)
    det_hash<> dets_C_hash;

    //double tau = time_step_;
    double variational_energy_estimator = 0.0;
    std::vector<double> energy(num_threads_,0.0);

    size_t max_I = dets.size();
    for (size_t I = 0; I < max_I; ++I){
        dets_C_hash[dets[I]] = C[I];
    }

    std::pair<double,double> zero(0.0,0.0);
#pragma omp parallel for
    for (size_t I = 0; I < max_I; ++I){
        int thread_id = omp_get_thread_num();
        // Update the list of couplings
        std::pair<double,double> max_coupling;
#pragma omp critical
        {
            max_coupling = dets_max_couplings_[dets[I]];
        }
        if (max_coupling == zero){
            max_coupling = {1.0,1.0};
        }
//        energy[thread_id] += form_H_C_sym(1.0,tollerance,dets[I],C[I],dets_C_hash,max_coupling);
        energy[thread_id] += form_H_C(1.0,tollerance,dets[I],C[I],dets_C_hash,max_coupling);
    }

    for (size_t I = 0; I < max_I; ++I){
        variational_energy_estimator += C[I] * C[I] * dets[I].energy();
    }
    for (int t = 0; t < num_threads_; ++t){
        variational_energy_estimator += energy[t];
    }

    return variational_energy_estimator + nuclear_repulsion_energy_;
}


void AdaptivePathIntegralCI::print_wfn(det_vec& space,std::vector<double>& C)
{
    outfile->Printf("\n\n  Most important contributions to the wave function:\n");

    std::vector<std::pair<double,size_t> > det_weight;
    for (size_t I = 0; I < space.size(); ++I){
        det_weight.push_back(std::make_pair(std::fabs(C[I]),I));
    }
    std::sort(det_weight.begin(),det_weight.end());
    std::reverse(det_weight.begin(),det_weight.end());
    size_t max_dets = std::min(10,int(C.size()));
    for (size_t I = 0; I < max_dets; ++I){
        outfile->Printf("\n  %3zu  %9.6f %.9f  %10zu %s",
                        I,
                        C[det_weight[I].second],
                        det_weight[I].first * det_weight[I].first,
                        det_weight[I].second,
                        space[det_weight[I].second].str().c_str());       
    }

    // Compute the expectation value of the spin
    size_t max_sample = 1000;
    size_t max_I = 0;
    double sum_weight = 0.0;
    double wfn_threshold = 0.95;
    for (size_t I = 0; I < space.size(); ++I){
        if ((sum_weight < wfn_threshold) and (I < max_sample)) {
            sum_weight += std::pow(det_weight[I].first,2.0);
            max_I++;
        }else if (std::fabs(det_weight[I].first - det_weight[I-1].first) < 1.0e-6){
            // Special case, if there are several equivalent determinants
            sum_weight += std::pow(det_weight[I].first,2.0);
            max_I++;
        }else{
            break;
        }
    }

    double norm = 0.0;
    double S2 = 0.0;
    for (size_t sI = 0; sI < max_I; ++sI){
        size_t I = det_weight[sI].second;
        for (size_t sJ = 0; sJ < max_I; ++sJ){
            size_t J = det_weight[sJ].second;
            if (std::fabs(C[I] * C[J]) > 1.0e-12){
                const double S2IJ = space[I].spin2(space[J]);
                S2 += C[I] * C[J] * S2IJ;
            }
        }
        norm += std::pow(C[I],2.0);
    }
    S2 /= norm;
    double S = std::fabs(0.5 * (std::sqrt(1.0 + 4.0 * S2) - 1.0));

    std::vector<string> s2_labels({"singlet","doublet","triplet","quartet","quintet","sextet","septet","octet","nonet","decaet"});
    std::string state_label = s2_labels[std::round(S * 2.0)];
    outfile->Printf("\n\n  Spin State: S^2 = %5.3f, S = %5.3f, %s (from %zu determinants,%.2f\%)",S2,S,state_label.c_str(),max_I,100.0 * sum_weight);

    outfile->Flush();
}

void AdaptivePathIntegralCI::save_wfn(det_vec& space,std::vector<double>& C,std::vector<det_hash<>>& solutions)
{
    outfile->Printf("\n\n  Saving the wave function:\n");

    det_hash<> solution;
    for (size_t I = 0; I < space.size(); ++I){
        solution[space[I]] = C[I];
    }
    solutions.push_back(std::move(solution));
}

void AdaptivePathIntegralCI::orthogonalize(det_vec& space,std::vector<double>& C,std::vector<det_hash<>>& solutions)
{
    det_hash<> det_C;
    for (size_t I = 0; I < space.size(); ++I){
        det_C[space[I]] = C[I];
    }
    for (size_t n = 0; n < solutions.size(); ++n){
        double dot_prod = dot(det_C,solutions[n]);
        add(det_C,-dot_prod,solutions[n]);
    }
    normalize(det_C);
    copy_hash_to_vec(det_C,space,C);
}

void combine_hashes(std::vector<det_hash<>>& thread_det_C_map,det_hash<>& dets_C_hash)
{
    // Combine the content of varius wave functions stored as maps
    for (size_t t = 0; t < thread_det_C_map.size(); ++t){
        for (det_hash_it it = thread_det_C_map[t].begin(), endit = thread_det_C_map[t].end(); it != endit; ++it){
            dets_C_hash[it->first] += it->second;
        }
    }
}

void combine_hashes(det_hash<>& dets_C_hash_A,det_hash<>& dets_C_hash_B)
{
    // Combine the content of varius wave functions stored as maps
    for (det_hash_it it = dets_C_hash_A.begin(), endit = dets_C_hash_A.end(); it != endit; ++it){
        dets_C_hash_B[it->first] += it->second;
    }
}

void combine_hashes_into_hash(std::vector<det_hash<>>& thread_det_C_hash,det_hash<>& dets_C_hash)
{
    // Combine the content of varius wave functions stored as maps
    for (size_t t = 0; t < thread_det_C_hash.size(); ++t){
        for (auto& kv : thread_det_C_hash[t]){
            dets_C_hash[kv.first] += kv.second;
        }
    }
}

void copy_hash_to_vec(det_hash<>& dets_C_hash,det_vec& dets,std::vector<double>& C)
{
    size_t size = dets_C_hash.size();
    dets.resize(size);
    C.resize(size);

    size_t I = 0;
    for (det_hash_it it = dets_C_hash.begin(), endit = dets_C_hash.end(); it != endit; ++it){
        dets[I] = it->first;
        C[I] = it->second;
        I++;
    }
}

double normalize(std::vector<double>& C)
{
    size_t size = C.size();
    double norm = 0.0;
    for (size_t I = 0; I < size; ++I){
        norm += C[I] * C[I];
    }
    norm = std::sqrt(norm);
    for (size_t I = 0; I < size; ++I){
        C[I] /= norm;
    }
    return norm;
}

double normalize(det_hash<>& dets_C)
{
    double norm = 0.0;
    for (auto& det_C : dets_C){
        norm += det_C.second * det_C.second;
    }
    norm = std::sqrt(norm);
    for (auto& det_C : dets_C){
        det_C.second /= norm;
    }
    return norm;
}

double norm(det_hash<>& dets_C)
{
    double norm = 0.0;
    for (auto& det_C : dets_C){
        norm += det_C.second * det_C.second;
    }
    norm = std::sqrt(norm);
    return norm;
}

double dot(det_hash<>& A,det_hash<>& B)
{
    double res = 0.0;
    for (auto& det_C : A){
        res += det_C.second * B[det_C.first];
    }
    return res;
}

void add(det_hash<>& A,double beta,det_hash<>& B)
{
    // A += beta B
    for (auto& det_C : B){
        A[det_C.first] += beta * det_C.second;
    }
}

void scale(std::vector<double>& A,double alpha)
{
    size_t size = A.size();
    for (size_t I = 0; I < size; ++I){
        A[I] *= alpha;
    }
}

void scale(det_hash<>& A,double alpha)
{
    for (auto& det_C : A){
        A[det_C.first] *= alpha;
    }
}

/*
double AdaptivePathIntegralCI::form_H_C_sym(double tau,double spawning_threshold,Determinant& detI, double CI, bit_hash& det_C,std::pair<double,double>& max_coupling)
{
    double result = 0.0;

    if ((max_coupling.second != 0.0) and (std::fabs(max_coupling.second  * CI) < spawning_threshold) and (std::fabs(max_coupling.first * CI) < spawning_threshold)){
        return result;
    }

    std::vector<std::vector<int>> naoccs(nirrep_), navirs(nirrep_), nboccs(nirrep_), nbvirs(nirrep_);

    for (int i = 0; i<ncmo_; i++) {
        if (detI.get_alfa_bit(i)) {
            naoccs[mo_symmetry_[i]].push_back(i);
        } else {
            navirs[mo_symmetry_[i]].push_back(i);
        }
        if (detI.get_beta_bit(i)) {
            nboccs[mo_symmetry_[i]].push_back(i);
        } else {
            nbvirs[mo_symmetry_[i]].push_back(i);
        }
    }

    size_t spawned = 0;

    // No diagonal contributions

    if ((std::fabs(max_coupling.first * CI) >= spawning_threshold)){
        for (int i = 0; i<nirrep_; i++) {
            for (int ix=0; ix<naoccs[i].size(); ix++) {
                int ii = naoccs[i][ix];
                for (int ax=0; ax<navirs[i].size(); ax++) {
                    int aa = navirs[i][ax];
                    Determinant detJ(detI);
                    detJ.set_alfa_bit(ii,false);
                    detJ.set_alfa_bit(aa,true);
                    det_hash_it it = det_C.find(detJ);
                    if (it != det_C.end()){
                        double HJI = detJ.slater_rules(detI);
                        if (std::fabs(HJI * CI) >= spawning_threshold){
                            result += tau * HJI * CI * it->second;
                            spawned++;
                        }
                    }
                    ndet_visited_++;
                }
            }
        }
        for (int i = 0; i<nirrep_; i++) {
            for (int ix=0; ix<nboccs[i].size(); ix++) {
                int ii = nboccs[i][ix];
                for (int ax=0; ax<nbvirs[i].size(); ax++) {
                    int aa = nbvirs[i][ax];
                    Determinant detJ(detI);
                    detJ.set_beta_bit(ii,false);
                    detJ.set_beta_bit(aa,true);
                    det_hash_it it = det_C.find(detJ);
                    if (it != det_C.end()){
                        double HJI = detJ.slater_rules(detI);
                        if (std::fabs(HJI * CI) >= spawning_threshold){
                            result += tau * HJI * CI * it->second;
                            spawned++;
                        }
                    }
                    ndet_visited_++;
                }
            }
        }
    }

    if ((max_coupling.second == 0.0) or (std::fabs(max_coupling.second  * CI) >= spawning_threshold)){
        for (int i = 0; i<nirrep_; i++) {
            for (int j = 0; j<nirrep_; j++) {
                for (int a = 0; a<nirrep_; a++){
                    int b = i^j^a;
                    for (int ix=0; ix<naoccs[i].size(); ix++) {
                        int ii = naoccs[i][ix];
                        for (int jx=0; jx<nboccs[j].size(); jx++) {
                            int jj = nboccs[j][jx];
                            for (int ax=0; ax<navirs[a].size(); ax++) {
                                int aa = navirs[a][ax];
                                for (int bx=0; bx<nbvirs[b].size(); bx++) {
                                    int bb = nbvirs[b][bx];
                                    double HJI = fci_ints_->tei_ab(ii,jj,aa,bb);

                                    if (std::fabs(HJI * CI) >= spawning_threshold){
                                        Determinant detJ(detI);
                                        double sign = detJ.double_excitation_ab(ii,jj,aa,bb);
//                                        detJ.set_alfa_bit(ii,false);
//                                        detJ.set_beta_bit(jj,false);
//                                        detJ.set_alfa_bit(aa,true);
//                                        detJ.set_beta_bit(bb,true);

                                        det_hash_it it = det_C.find(detJ);
                                        if (it != det_C.end()){
//                                            // grap the alpha bits of both determinants
//                                            const Determinant::bit_t& Ia = detI.alfa_bits();
//                                            const Determinant::bit_t& Ib = detI.beta_bits();
//                                            const Determinant::bit_t& Ja = detJ.alfa_bits();
//                                            const Determinant::bit_t& Jb = detJ.beta_bits();

                                            // compute the sign of the matrix element
//                                            HJI *= Determinant::SlaterSign(Ia,ii) * Determinant::SlaterSign(Ib,jj) * Determinant::SlaterSign(Ja,aa) * Determinant::SlaterSign(Jb,bb);

                                            result += sign * tau * HJI * CI * it->second;

                                        }
                                    }
                                    ndet_visited_++;

                                }
                            }
                        }
                    }
                }
            }
        }
        for (int i = 0; i<nirrep_; i++) {
            for (int a = 0; a<nirrep_; a++) {
                for (int ix=0; ix<naoccs[i].size(); ix++) {
                    int ii = naoccs[i][ix];
                    for (int jx=ix+1; jx<naoccs[i].size(); jx++) {
                        int jj = naoccs[i][jx];
                        for (int ax=0; ax<navirs[a].size(); ax++) {
                            int aa = navirs[a][ax];
                            for (int bx=ax+1; bx<navirs[a].size(); bx++) {
                                int bb = navirs[a][bx];
                                double HJI = fci_ints_->tei_aa(ii,jj,aa,bb);

                                if (std::fabs(HJI * CI) >= spawning_threshold){
                                    Determinant detJ(detI);
                                    double sign = detJ.double_excitation_aa(ii,jj,aa,bb);

//                                    detJ.set_alfa_bit(ii,false);
//                                    detJ.set_alfa_bit(jj,false);
//                                    detJ.set_alfa_bit(aa,true);
//                                    detJ.set_alfa_bit(bb,true);

                                    det_hash_it it = det_C.find(detJ);
                                    if (it != det_C.end()){
//                                        // grap the alpha bits of both determinants
//                                        const Determinant::bit_t& Ia = detI.alfa_bits();
//                                        const Determinant::bit_t& Ja = detJ.alfa_bits();

//                                        // compute the sign of the matrix element
//                                        HJI *= Determinant::SlaterSign(Ia,ii) * Determinant::SlaterSign(Ia,jj) * Determinant::SlaterSign(Ja,aa) * Determinant::SlaterSign(Ja,bb);

                                        result += sign * tau * HJI * CI * it->second;

                                    }
                                }
                                ndet_visited_++;
                            }
                        }
                    }
                }
            }
        }
        for (int i = 0; i<nirrep_; i++) {
            for (int j = i+1; j<nirrep_; j++) {
                for (int a = 0; a<nirrep_; a++){
                    int b = i^j^a;
                    if (b > a) {
                        for (int ix=0; ix<naoccs[i].size(); ix++) {
                            int ii = naoccs[i][ix];
                            for (int jx=0; jx<naoccs[j].size(); jx++) {
                                int jj = naoccs[j][jx];
                                for (int ax=0; ax<navirs[a].size(); ax++) {
                                    int aa = navirs[a][ax];
                                    for (int bx=0; bx<navirs[b].size(); bx++) {
                                        int bb = navirs[b][bx];
                                        double HJI = fci_ints_->tei_aa(ii,jj,aa,bb);

                                        if (std::fabs(HJI * CI) >= spawning_threshold){
                                            Determinant detJ(detI);
                                            detJ.set_alfa_bit(ii,false);
                                            detJ.set_alfa_bit(jj,false);
                                            detJ.set_alfa_bit(aa,true);
                                            detJ.set_alfa_bit(bb,true);

                                            det_hash_it it = det_C.find(detJ);
                                            if (it != det_C.end()){
                                                // grap the alpha bits of both determinants
                                                const Determinant::bit_t& Ia = detI.alfa_bits();
                                                const Determinant::bit_t& Ja = detJ.alfa_bits();

                                                // compute the sign of the matrix element
                                                HJI *= Determinant::SlaterSign(Ia,ii) * Determinant::SlaterSign(Ia,jj) * Determinant::SlaterSign(Ja,aa) * Determinant::SlaterSign(Ja,bb);

                                                result += tau * HJI * CI * it->second;

                                            }
                                        }
                                        ndet_visited_++;
                                    }
                                }
                            }
                        }
                    }

                }
            }
        }
        for (int i = 0; i<nirrep_; i++) {
            for (int a = 0; a<nirrep_; a++) {
                for (int ix=0; ix<nboccs[i].size(); ix++) {
                    int ii = nboccs[i][ix];
                    for (int jx=ix+1; jx<nboccs[i].size(); jx++) {
                        int jj = nboccs[i][jx];
                        for (int ax=0; ax<nbvirs[a].size(); ax++) {
                            int aa = nbvirs[a][ax];
                            for (int bx=ax+1; bx<nbvirs[a].size(); bx++) {
                                int bb = nbvirs[a][bx];
                                double HJI = fci_ints_->tei_bb(ii,jj,aa,bb);

                                if (std::fabs(HJI * CI) >= spawning_threshold){
                                    Determinant detJ(detI);
                                    detJ.set_beta_bit(ii,false);
                                    detJ.set_beta_bit(jj,false);
                                    detJ.set_beta_bit(aa,true);
                                    detJ.set_beta_bit(bb,true);

                                    det_hash_it it = det_C.find(detJ);
                                    if (it != det_C.end()){
                                        // grap the alpha bits of both determinants
                                        const Determinant::bit_t& Ib = detI.beta_bits();
                                        const Determinant::bit_t& Jb = detJ.beta_bits();

                                        // compute the sign of the matrix element
                                        HJI *= Determinant::SlaterSign(Ib,ii) * Determinant::SlaterSign(Ib,jj) * Determinant::SlaterSign(Jb,aa) * Determinant::SlaterSign(Jb,bb);

                                        result += tau * HJI * CI * it->second;

                                    }
                                }
                                ndet_visited_++;
                            }
                        }
                    }
                }
            }
        }
        for (int i = 0; i<nirrep_; i++) {
            for (int j = i+1; j<nirrep_; j++) {
                for (int a = 0; a<nirrep_; a++){
                    int b = i^j^a;
                    if (b > a) {
                        for (int ix=0; ix<nboccs[i].size(); ix++) {
                            int ii = nboccs[i][ix];
                            for (int jx=0; jx<nboccs[j].size(); jx++) {
                                int jj = nboccs[j][jx];
                                for (int ax=0; ax<nbvirs[a].size(); ax++) {
                                    int aa = nbvirs[a][ax];
                                    for (int bx=0; bx<nbvirs[b].size(); bx++) {
                                        int bb = nbvirs[b][bx];
                                        double HJI = fci_ints_->tei_bb(ii,jj,aa,bb);

                                        if (std::fabs(HJI * CI) >= spawning_threshold){
                                            Determinant detJ(detI);
                                            detJ.set_beta_bit(ii,false);
                                            detJ.set_beta_bit(jj,false);
                                            detJ.set_beta_bit(aa,true);
                                            detJ.set_beta_bit(bb,true);

                                            det_hash_it it = det_C.find(detJ);
                                            if (it != det_C.end()){
                                                // grap the alpha bits of both determinants
                                                const Determinant::bit_t& Ib = detI.beta_bits();
                                                const Determinant::bit_t& Jb = detJ.beta_bits();

                                                // compute the sign of the matrix element
                                                HJI *= Determinant::SlaterSign(Ib,ii) * Determinant::SlaterSign(Ib,jj) * Determinant::SlaterSign(Jb,aa) * Determinant::SlaterSign(Jb,bb);

                                                result += tau * HJI * CI * it->second;

                                            }
                                        }
                                        ndet_visited_++;
                                    }
                                }
                            }
                        }
                    }

                }
            }
        }
    }

    return result;
}
*/


double AdaptivePathIntegralCI::form_H_C(double tau,double spawning_threshold,Determinant& detI, double CI, det_hash<>& det_C,std::pair<double,double>& max_coupling)
{
    double result = 0.0;

    std::vector<int> aocc = detI.get_alfa_occ();
    std::vector<int> bocc = detI.get_beta_occ();
    std::vector<int> avir = detI.get_alfa_vir();
    std::vector<int> bvir = detI.get_beta_vir();

    int noalpha = aocc.size();
    int nobeta  = bocc.size();
    int nvalpha = avir.size();
    int nvbeta  = bvir.size();

    // No diagonal contributions

    if ((std::fabs(max_coupling.first * CI) >= spawning_threshold)){
        // Generate aa excitations
        for (int i = 0; i < noalpha; ++i){
            int ii = aocc[i];
            for (int a = 0; a < nvalpha; ++a){
                int aa = avir[a];
                if ((mo_symmetry_[ii] ^ mo_symmetry_[aa]) == 0){
                    Determinant detJ(detI);
                    detJ.set_alfa_bit(ii,false);
                    detJ.set_alfa_bit(aa,true);
                    det_hash_it it = det_C.find(detJ);
                    if (it != det_C.end()){
                        double HJI = detJ.slater_rules(detI);
                        if (std::fabs(HJI * CI) >= spawning_threshold){
                            result += tau * HJI * CI * it->second;
                        }
                    }
                }
            }
        }

        for (int i = 0; i < nobeta; ++i){
            int ii = bocc[i];
            for (int a = 0; a < nvbeta; ++a){
                int aa = bvir[a];
                if ((mo_symmetry_[ii] ^ mo_symmetry_[aa])  == 0){
                    Determinant detJ(detI);
                    detJ.set_beta_bit(ii,false);
                    detJ.set_beta_bit(aa,true);
                    det_hash_it it = det_C.find(detJ);
                    if (it != det_C.end()){
                        double HJI = detJ.slater_rules(detI);
                        if (std::fabs(HJI * CI) >= spawning_threshold){
                            result += tau * HJI * CI * it->second;
                        }
                    }
                }
            }
        }
    }

    if ((max_coupling.second == 0.0) or (std::fabs(max_coupling.second  * CI) >= spawning_threshold)){
        // Generate aa excitations
        for (int i = 0; i < noalpha; ++i){
            int ii = aocc[i];
            for (int j = i + 1; j < noalpha; ++j){
                int jj = aocc[j];
                for (int a = 0; a < nvalpha; ++a){
                    int aa = avir[a];
                    for (int b = a + 1; b < nvalpha; ++b){
                        int bb = avir[b];
                        if ((mo_symmetry_[ii] ^ mo_symmetry_[jj] ^ mo_symmetry_[aa] ^ mo_symmetry_[bb]) == 0){
                            double HJI = fci_ints_->tei_aa(ii,jj,aa,bb);

                            if (std::fabs(HJI * CI) >= spawning_threshold){
                                Determinant detJ(detI);
                                double sign = detJ.double_excitation_aa(ii,jj,aa,bb);
                                det_hash_it it = det_C.find(detJ);
                                if (it != det_C.end()){
                                    result += sign * tau * HJI * CI * it->second;
                                }
                            }
                        }
                    }
                }
            }
        }

        for (int i = 0; i < noalpha; ++i){
            int ii = aocc[i];
            for (int j = 0; j < nobeta; ++j){
                int jj = bocc[j];
                for (int a = 0; a < nvalpha; ++a){
                    int aa = avir[a];
                    for (int b = 0; b < nvbeta; ++b){
                        int bb = bvir[b];
                        if ((mo_symmetry_[ii] ^ mo_symmetry_[jj] ^ mo_symmetry_[aa] ^ mo_symmetry_[bb]) == 0){
                            double HJI = fci_ints_->tei_ab(ii,jj,aa,bb);

                            if (std::fabs(HJI * CI) >= spawning_threshold){
                                Determinant detJ(detI);
                                double sign = detJ.double_excitation_ab(ii,jj,aa,bb);
                                det_hash_it it = det_C.find(detJ);
                                if (it != det_C.end()){
                                    result += sign * tau * HJI * CI * it->second;
                                }
                            }
                        }
                    }
                }
            }
        }
        for (int i = 0; i < nobeta; ++i){
            int ii = bocc[i];
            for (int j = i + 1; j < nobeta; ++j){
                int jj = bocc[j];
                for (int a = 0; a < nvbeta; ++a){
                    int aa = bvir[a];
                    for (int b = a + 1; b < nvbeta; ++b){
                        int bb = bvir[b];
                        if ((mo_symmetry_[ii] ^ (mo_symmetry_[jj] ^ (mo_symmetry_[aa] ^ mo_symmetry_[bb]))) == 0){
                            double HJI = fci_ints_->tei_bb(ii,jj,aa,bb);
                            if (std::fabs(HJI * CI) >= spawning_threshold){
                                Determinant detJ(detI);
                                double sign = detJ.double_excitation_bb(ii,jj,aa,bb);
                                det_hash_it it = det_C.find(detJ);
                                if (it != det_C.end()){
                                    result += sign * tau * HJI * CI * it->second;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return result;
}

}} // EndNamespaces
