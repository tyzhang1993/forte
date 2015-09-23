#include <psi4-dec.h>
#include <libmoinfo/libmoinfo.h>
#include <libmints/matrix.h>

#include <boost/lexical_cast.hpp>

#include "bitset_determinant.h"
#include "fci_vector.h"

using namespace std;
using namespace psi;

namespace psi{ namespace forte{

std::size_t hash_value(const BitsetDeterminant& input)
{
    return (input.alfa_bits_.to_ulong() % 100000 + input.beta_bits_.to_ulong() % 100000);
}

// Static members
std::vector<BitsetDeterminant::bit_t> BitsetDeterminant::bit_mask_;
std::shared_ptr<FCIIntegrals> BitsetDeterminant::fci_ints_;

void BitsetDeterminant::set_ints(std::shared_ptr<FCIIntegrals> ints)
{
    fci_ints_ = ints;

    // Initialize the bit masks
    int n = ints->nmo();
    outfile->Printf("\n  Generating bit mask for the Bitset class (nmo = %d)",n);

    bit_mask_.clear();
    for (int i = 0; i < n; ++i){
        bit_t b(n);
        for (int j = 0; j < i; ++j){
            b[j] = 1;
        }
        bit_mask_.push_back(b);
    }
}

BitsetDeterminant::BitsetDeterminant() : nmo_(0)
{
}

/// Construct the determinant from an occupation vector that
/// specifies the alpha and beta strings.  occupation = [Ia,Ib]
BitsetDeterminant::BitsetDeterminant(const std::vector<int>& occupation,bool print_det)
    : nmo_(occupation.size() / 2), alfa_bits_(nmo_), beta_bits_(nmo_)
{
    for(int p = 0; p < nmo_; ++p){
        alfa_bits_[p] = occupation[p];
        beta_bits_[p] = occupation[p + nmo_];
    }
    if (print_det) print();
}

/// Construct the determinant from an occupation vector that
/// specifies the alpha and beta strings.  occupation = [Ia,Ib]
BitsetDeterminant::BitsetDeterminant(const std::vector<bool>& occupation,bool print_det)
    : nmo_(occupation.size() / 2), alfa_bits_(nmo_), beta_bits_(nmo_)
{
    for(int p = 0; p < nmo_; ++p){
        alfa_bits_[p] = occupation[p];
        beta_bits_[p] = occupation[p + nmo_];
    }
    if (print_det) print();
}

BitsetDeterminant::BitsetDeterminant(const std::vector<bool>& occupation_a,const std::vector<bool>& occupation_b,bool print_det)
    : nmo_(occupation_a.size()), alfa_bits_(nmo_), beta_bits_(nmo_)
{
    for(int p = 0; p < nmo_; ++p){
        alfa_bits_[p] = occupation_a[p];
        beta_bits_[p] = occupation_b[p];
    }
    if (print_det) print();
}

std::vector<int> BitsetDeterminant::get_alfa_occ()
{
    std::vector<int> occ(alfa_bits_.count());
    size_t index = alfa_bits_.find_first();
    int i = 0;
    while(index != boost::dynamic_bitset<>::npos)
    {
            occ[i] = index;
            index = alfa_bits_.find_next(index);
            i++;
    }
    return occ;
}

std::vector<int> BitsetDeterminant::get_beta_occ()
{
    std::vector<int> occ(beta_bits_.count());
    size_t index = beta_bits_.find_first();
    int i = 0;
    while(index != boost::dynamic_bitset<>::npos)
    {
            occ[i] = index;
            index = beta_bits_.find_next(index);
            i++;
    }
    return occ;
}

std::vector<int> BitsetDeterminant::get_alfa_vir()
{
    alfa_bits_.flip();
    std::vector<int> vir(alfa_bits_.count());
    size_t index = alfa_bits_.find_first();
    int i = 0;
    while(index != boost::dynamic_bitset<>::npos)
    {
            vir[i] = index;
            index = alfa_bits_.find_next(index);
            i++;
    }
    alfa_bits_.flip();
    return vir;
}

std::vector<int> BitsetDeterminant::get_beta_vir()
{
    beta_bits_.flip();
    std::vector<int> vir(beta_bits_.count());
    size_t index = beta_bits_.find_first();
    int i = 0;
    while(index != boost::dynamic_bitset<>::npos)
    {
            vir[i] = index;
            index = beta_bits_.find_next(index);
            i++;
    }
    beta_bits_.flip();
    return vir;
}


std::vector<int> BitsetDeterminant::get_alfa_occ() const
{
    std::vector<int> occ(alfa_bits_.count());
    size_t index = alfa_bits_.find_first();
    int i = 0;
    while(index != boost::dynamic_bitset<>::npos)
    {
            occ[i] = index;
            index = alfa_bits_.find_next(index);
            i++;
    }
    return occ;
}

std::vector<int> BitsetDeterminant::get_beta_occ() const
{
    std::vector<int> occ(beta_bits_.count());
    size_t index = beta_bits_.find_first();
    int i = 0;
    while(index != boost::dynamic_bitset<>::npos)
    {
            occ[i] = index;
            index = beta_bits_.find_next(index);
            i++;
    }
    return occ;
}

std::vector<int> BitsetDeterminant::get_alfa_vir() const
{
    boost::dynamic_bitset<> alfa_bits(alfa_bits_);
    alfa_bits.flip();
    std::vector<int> vir(alfa_bits.count());
    size_t index = alfa_bits.find_first();
    int i = 0;
    while(index != boost::dynamic_bitset<>::npos)
    {
            vir[i] = index;
            index = alfa_bits.find_next(index);
            i++;
    }
    return vir;
}

std::vector<int> BitsetDeterminant::get_beta_vir() const
{
    boost::dynamic_bitset<> beta_bits(beta_bits_);
    beta_bits.flip();
    std::vector<int> vir(beta_bits.count());
    size_t index = beta_bits.find_first();
    int i = 0;
    while(index != boost::dynamic_bitset<>::npos)
    {
            vir[i] = index;
            index = beta_bits.find_next(index);
            i++;
    }
    return vir;
}

double BitsetDeterminant::create_alfa_bit(int n)
{
    if (alfa_bits_[n]) return 0.0;
    alfa_bits_[n] = true;
    return SlaterSign(alfa_bits_,n);
}

double BitsetDeterminant::create_beta_bit(int n)
{
    if (beta_bits_[n]) return 0.0;
    beta_bits_[n] = true;
    return SlaterSign(beta_bits_,n);
}

double BitsetDeterminant::destroy_alfa_bit(int n)
{
    if (not alfa_bits_[n]) return 0.0;
    alfa_bits_[n] = false;
    return SlaterSign(alfa_bits_,n);
}

/// Set the value of a beta bit
double BitsetDeterminant::destroy_beta_bit(int n)
{
    if (not beta_bits_[n]) return 0.0;
    beta_bits_[n] = false;
    return SlaterSign(beta_bits_,n);
}

/// Switch alfa and beta bits
void BitsetDeterminant::spin_flip()
{
    std::swap(alfa_bits_,beta_bits_);
}

/**
 * Print the determinant
 */
void BitsetDeterminant::print() const
{
    outfile->Printf("\n  |");
    for(int p = 0; p < nmo_; ++p){
        outfile->Printf("%d",alfa_bits_[p] ? 1 :0);
    }
    outfile->Printf("|");
    for(int p = 0; p < nmo_; ++p){
        outfile->Printf("%d",beta_bits_[p] ? 1 :0);
    }
    outfile->Printf(">");
    outfile->Flush();
}

/**
 * Print the determinant
 */
std::string BitsetDeterminant::str() const
{
    std::string s;
    s += "|";
    for(int p = 0; p < nmo_; ++p){
        s += boost::lexical_cast<std::string>(alfa_bits_[p]);
    }
    s += "|";
    for(int p = 0; p < nmo_; ++p){
        s += boost::lexical_cast<std::string>(beta_bits_[p]);
    }
    s += ">";
    return s;
}

/**
 * Compute the energy of this determinant
 * @return the electronic energy (does not include the nuclear repulsion energy)
 */
double BitsetDeterminant::energy() const
{
    double matrix_element = 0.0;
    matrix_element = fci_ints_->frozen_core_energy();
    //    for(int p = 0; p < nmo_; ++p){
    //        if(alfa_bits_[p]) matrix_element += ints_->diag_roei(p);
    //        if(beta_bits_[p]) matrix_element += ints_->diag_roei(p);
    //        if(alfa_bits_[p]) outfile->Printf("\n  One-electron terms: %20.12f + %20.12f (string)",ints_->diag_roei(p),ints_->diag_roei(p));
    //        for(int q = 0; q < nmo_; ++q){
    //            if(alfa_bits_[p] and alfa_bits_[q]){
    //                matrix_element +=   0.5 * ints_->diag_ce_rtei(p,q);
    //                outfile->Printf("\n  One-electron terms (%da,%da): 0.5 * %20.12f (string)",p,q,ints_->diag_ce_rtei(p,q));
    //            }
    //            if(alfa_bits_[p] and beta_bits_[q]){
    //                matrix_element += ints_->diag_c_rtei(p,q);
    //                outfile->Printf("\n  One-electron terms (%da,%db): 1.0 * %20.12f (string)",p,q,ints_->diag_c_rtei(p,q));
    //            }
    //            if(beta_bits_[p] and beta_bits_[q]){
    //                matrix_element +=   0.5 * fci_ints_->diag_ce_rtei(p,q);
    //                outfile->Printf("\n  One-electron terms (%db,%db): 0.5 * %20.12f (string)",p,q,ints_->diag_ce_rtei(p,q));
    //            }
    //        }
    //    }
    for(int p = 0; p < nmo_; ++p){
        if(alfa_bits_[p]) matrix_element += fci_ints_->oei_a(p,p);
        if(beta_bits_[p]) matrix_element += fci_ints_->oei_b(p,p);
             //   if(alfa_bits_[p]) outfile->Printf("\n  One-electron terms: %20.12f + %20.12f (string)",fci_ints_->oei_a(p,p),fci_ints_->oei_a(p,p));

        for(int q = 0; q < nmo_; ++q){
            if(alfa_bits_[p] and alfa_bits_[q]){
                matrix_element += 0.5 *  fci_ints_->diag_tei_aa(p,q);
               //                 outfile->Printf("\n  Two-electron terms (%da,%da): 0.5 * %20.12f (string)",p,q,fci_ints_->diag_tei_aa(p,q));
            }
            if(alfa_bits_[p] and beta_bits_[q]){
                matrix_element +=  fci_ints_->diag_tei_ab(p,q);
                 //               outfile->Printf("\n  Two-electron terms (%da,%db): 1.0 * %20.12f (string)",p,q,fci_ints_->diag_tei_ab(p,q));
            }
            if(beta_bits_[p] and beta_bits_[q]){
                matrix_element += 0.5 *  fci_ints_->diag_tei_bb(p,q);
                   //             outfile->Printf("\n  Two-electron terms (%db,%db): 0.5 * %20.12f (string)",p,q,fci_ints_->diag_tei_bb(p,q));
            }
        }
    }
    return(matrix_element);
}

/**
 * Compute the matrix element of the Hamiltonian between this determinant and a given one
 * @param rhs
 * @return
 */
double BitsetDeterminant::slater_rules(const BitsetDeterminant& rhs) const
{
    const boost::dynamic_bitset<>& Ia = alfa_bits_;
    const boost::dynamic_bitset<>& Ib = beta_bits_;
    const boost::dynamic_bitset<>& Ja = rhs.alfa_bits_;
    const boost::dynamic_bitset<>& Jb = rhs.beta_bits_;


    int nadiff = 0;
    int nbdiff = 0;
    // Count how many differences in mos are there
    for (int n = 0; n < nmo_; ++n) {
        if (Ia[n] != Ja[n]) nadiff++;
        if (Ib[n] != Jb[n]) nbdiff++;
        if (nadiff + nbdiff > 4) return 0.0; // Get out of this as soon as possible
    }
    nadiff /= 2;
    nbdiff /= 2;

    double matrix_element = 0.0;
    // Slater rule 1 PhiI = PhiJ
    if ((nadiff == 0) and (nbdiff == 0)) {
        matrix_element = fci_ints_->frozen_core_energy();
        for(int p = 0; p < nmo_; ++p){
            if(alfa_bits_[p]) matrix_element += fci_ints_->oei_a(p,p);
            if(beta_bits_[p]) matrix_element += fci_ints_->oei_b(p,p);
            for(int q = 0; q < nmo_; ++q){
                if(alfa_bits_[p] and alfa_bits_[q])
                    matrix_element +=   0.5 * fci_ints_->diag_tei_aa(p,q);
                //                    matrix_element +=   0.5 * ints_->diag_ce_rtei(p,q);
                if(beta_bits_[p] and beta_bits_[q])
                    matrix_element +=   0.5 * fci_ints_->diag_tei_bb(p,q);
                //                    matrix_element +=   0.5 * ints_->diag_ce_rtei(p,q);
                if(alfa_bits_[p] and beta_bits_[q])
                    matrix_element +=   fci_ints_->diag_tei_ab(p,q);
                //                    matrix_element += fci_ints_->diag_c_rtei(p,q);
            }
        }
    }

    // Slater rule 2 PhiI = j_a^+ i_a PhiJ
    if ((nadiff == 1) and (nbdiff == 0)) {
        // Diagonal contribution
        int i = 0;
        int j = 0;
        for(int p = 0; p < nmo_; ++p){
            if((Ia[p] != Ja[p]) and Ia[p]) i = p;
            if((Ia[p] != Ja[p]) and Ja[p]) j = p;
        }
        double sign = SlaterSign(Ia,i) * SlaterSign(Ja,j);
        matrix_element = sign * fci_ints_->oei_a(i,j);
        for(int p = 0; p < nmo_; ++p){
            if(Ia[p] and Ja[p]){
                matrix_element += sign * fci_ints_->tei_aa(i,p,j,p);
            }
            if(Ib[p] and Jb[p]){
                matrix_element += sign * fci_ints_->tei_ab(i,p,j,p);
            }
        }
    }
    // Slater rule 2 PhiI = j_b^+ i_b PhiJ
    if ((nadiff == 0) and (nbdiff == 1)) {
        // Diagonal contribution
        int i = 0;
        int j = 0;
        for(int p = 0; p < nmo_; ++p){
            if((Ib[p] != Jb[p]) and Ib[p]) i = p;
            if((Ib[p] != Jb[p]) and Jb[p]) j = p;
        }
        double sign = SlaterSign(Ib,i) * SlaterSign(Jb,j);
        matrix_element = sign * fci_ints_->oei_b(i,j);
        for(int p = 0; p < nmo_; ++p){
            if(Ia[p] and Ja[p]){
                matrix_element += sign * fci_ints_->tei_ab(p,i,p,j);
            }
            if(Ib[p] and Jb[p]){
                matrix_element += sign * fci_ints_->tei_bb(i,p,j,p);
            }
        }
    }

    // Slater rule 3 PhiI = k_a^+ l_a^+ j_a i_a PhiJ
    if ((nadiff == 2) and (nbdiff == 0)) {
        // Diagonal contribution
        int i = -1;
        int j =  0;
        int k = -1;
        int l =  0;
        for(int p = 0; p < nmo_; ++p){
            if((Ia[p] != Ja[p]) and Ia[p]){
                if (i == -1) { i = p; } else { j = p; }
            }
            if((Ia[p] != Ja[p]) and Ja[p]){
                if (k == -1) { k = p; } else { l = p; }
            }
        }
        double sign = SlaterSign(Ia,i) * SlaterSign(Ia,j) * SlaterSign(Ja,k) * SlaterSign(Ja,l);
        matrix_element = sign * fci_ints_->tei_aa(i,j,k,l);
    }

    // Slater rule 3 PhiI = k_a^+ l_a^+ j_a i_a PhiJ
    if ((nadiff == 0) and (nbdiff == 2)) {
        // Diagonal contribution
        int i,j,k,l;
        i = -1;
        j = -1;
        k = -1;
        l = -1;
        for(int p = 0; p < nmo_; ++p){
            if((Ib[p] != Jb[p]) and Ib[p]){
                if (i == -1) { i = p; } else { j = p; }
            }
            if((Ib[p] != Jb[p]) and Jb[p]){
                if (k == -1) { k = p; } else { l = p; }
            }
        }
        double sign = SlaterSign(Ib,i) * SlaterSign(Ib,j) * SlaterSign(Jb,k) * SlaterSign(Jb,l);
        matrix_element = sign * fci_ints_->tei_bb(i,j,k,l);
    }

    // Slater rule 3 PhiI = j_a^+ i_a PhiJ
    if ((nadiff == 1) and (nbdiff == 1)) {
        // Diagonal contribution
        int i,j,k,l;
        i = j = k = l = -1;
        for(int p = 0; p < nmo_; ++p){
            if((Ia[p] != Ja[p]) and Ia[p]) i = p;
            if((Ib[p] != Jb[p]) and Ib[p]) j = p;
            if((Ia[p] != Ja[p]) and Ja[p]) k = p;
            if((Ib[p] != Jb[p]) and Jb[p]) l = p;
        }
        double sign = SlaterSign(Ia,i) * SlaterSign(Ib,j) * SlaterSign(Ja,k) * SlaterSign(Jb,l);
        matrix_element = sign * fci_ints_->tei_ab(i,j,k,l);
    }
    return(matrix_element);
}

/**
 * Compute the S^2 matrix element of the Hamiltonian between two determinants specified by the strings (Ia,Ib) and (Ja,Jb)
 * @return S^2
 */
double BitsetDeterminant::spin2(const BitsetDeterminant& rhs) const
{
    const boost::dynamic_bitset<>& Ia = alfa_bits_;
    const boost::dynamic_bitset<>& Ib = beta_bits_;
    const boost::dynamic_bitset<>& Ja = rhs.alfa_bits_;
    const boost::dynamic_bitset<>& Jb = rhs.beta_bits_;

    // Compute the matrix elements of the operator S^2
    // S^2 = S- S+ + Sz (Sz + 1)
    //     = Sz (Sz + 1) + Nbeta + Npairs - sum_pq' a+(qa) a+(pb) a-(qb) a-(pa)
    double matrix_element = 0.0;

    int nadiff = 0;
    int nbdiff = 0;
    int na = 0;
    int nb = 0;
    int npair = 0;
    int nmo = nmo_;
    // Count how many differences in mos are there and the number of alpha/beta electrons
    for (int n = 0; n < nmo; ++n) {
        if (Ia[n] != Ja[n]) nadiff++;
        if (Ib[n] != Jb[n]) nbdiff++;
        if (Ia[n]) na++;
        if (Ib[n]) nb++;
        if ((Ia[n] and Ib[n])) npair += 1;
    }
    nadiff /= 2;
    nbdiff /= 2;

    double Ms = 0.5 * static_cast<double>(na - nb);

    // PhiI = PhiJ -> S^2 = Sz (Sz + 1) + Nbeta + Npairs
    if ((nadiff == 0) and (nbdiff == 0)) {
        matrix_element += Ms * (Ms + 1.0) + double(nb) - double(npair);
    }

    // PhiI = a+(qa) a+(pb) a-(qb) a-(pa) PhiJ
    if ((nadiff == 1) and (nbdiff == 1)) {
        // Find a pair of spin coupled electrons
        int i = -1;
        int j = -1;
        // The logic here is a bit complex
        for(int p = 0; p < nmo; ++p){
            if(Ja[p] and Ib[p] and (not Jb[p]) and  (not Ia[p])) i = p; //(p)
            if(Jb[p] and Ia[p] and (not Ja[p]) and  (not Ib[p])) j = p; //(q)
        }
        if (i != j and i >= 0 and j >= 0){
            double sign = SlaterSign(Ja,i) * SlaterSign(Jb,j) * SlaterSign(Ib,i) * SlaterSign(Ia,j);
            matrix_element -= sign;
        }
    }
    return(matrix_element);
}

double BitsetDeterminant::SlaterSign(const boost::dynamic_bitset<>& I,int n)
{
    double sign = 1.0;
    for(int i = 0; i < n; ++i){  // This runs up to the operator before n
        if(I[i]) sign *= -1.0;
    }
    return(sign);
}

double BitsetDeterminant::FastSlaterSign(const boost::dynamic_bitset<>& I,int n)
{
    return ((bit_mask_[n] & I).count() % 2) ? -1.0 : 1.0;
}

void BitsetDeterminant::check_uniqueness(const std::vector<BitsetDeterminant> det_space)
{
	size_t duplicates = 0;
	size_t dim = det_space.size();
	std::unordered_map<BitsetDeterminant, size_t, function<decltype(hash_value)>> det_map(dim, hash_value);
	
	for(const auto &i : det_space){
		++det_map[i];		
	}
	for(const auto &d : det_map){
		if(d.second > 1){
			outfile->Printf("\n  Duplicate determinant! ==> %s", d.first.str().c_str() );
			duplicates += d.second;
		}
	}
	outfile->Printf("\n  Number of duplicate determinants:  %zu  ", duplicates);
}

}} // end namespace
