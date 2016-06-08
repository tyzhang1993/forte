/*
 *@BEGIN LICENSE
 *
 * Libadaptive: an ab initio quantum chemistry software package
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 *@END LICENSE
 */

#ifndef _sparse_ci_wfn_h_
#define _sparse_ci_wfn_h_

#include "stl_bitset_determinant.h"


namespace psi{ namespace forte{

/**
 * @brief A class to store sparse configuration interaction wave functions
 * Stores a hash of determinants.
 */

using wfn_hash = det_hash<double>;

class SparseCIWavefunction
{
public:

    /// Default constructor
    SparseCIWavefunction( std::vector<STLBitsetDeterminant>& dets, std::vector<double>& cI );

    /// Copy constructor
    SparseCIWavefunction(wfn_hash& wfn_);

    /// @return The hash
    wfn_hash& wfn();

    /// Scale the wavefunction
    void scale( double value );

    /// Return the norm of the wavefunction
    double wfn_norm();

    /// Normalize the wavefunction
    void normalize();

    /// Add a determinant
    void add( STLBitsetDeterminant& det, double value );

    /// Merge two wavefunctions, overwriting the original in the case of conflicts
    void merge( SparseCIWavefunction& wfn ); 

    /// Print the most important determinants
    void print();
protected:

    /// The dimension of the hash
    size_t wfn_size_;

    /// A hash of (determinants,coefficients)
    wfn_hash wfn_;
};

}}

#endif // _sparse_ci_wfn_h_
