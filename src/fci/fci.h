/*
 * @BEGIN LICENSE
 *
 * Forte: an open-source plugin to Psi4 (https://github.com/psi4/psi4)
 * that implements a variety of quantum chemistry methods for strongly
 * correlated electrons.
 *
 * Copyright (c) 2012-2017 by its authors (see LICENSE, AUTHORS).
 *
 * The copyrights for code used from other parties are included in
 * the corresponding files.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 *
 * @END LICENSE
 */

#ifndef _fci_h_
#define _fci_h_

#include "psi4/libmints/wavefunction.h"
#include "psi4/physconst.h"

#include "../helpers.h"
#include "../integrals.h"
#include "../string_lists.h"
#include "../reference.h"
#include "../forte_options.h"
#include "fci_solver.h"

namespace psi {
namespace forte {

/// Set the options for the FCI method
void set_FCI_options(ForteOptions& foptions);

/**
 * @brief The FCI class
 * This class implements a FCI wave function and calls FCISolver
 */
class FCI : public Wavefunction {
  public:
    // ==> Class Constructor and Destructor <==

    /**
     * Constructor
     * @param ref_wfn The reference wavefunction object
     * @param options The main options object
     * @param ints A pointer to an allocated integral object
     * @param mo_space_info A pointer to the MOSpaceInfo object
     */
    FCI(SharedWavefunction ref_wfn, Options& options,
        std::shared_ptr<ForteIntegrals> ints,
        std::shared_ptr<MOSpaceInfo> mo_space_info);

    ~FCI();

    // ==> Class Interface <==

    /// Compute the energy
    virtual double compute_energy();
    /// Return a reference object
    Reference reference();
    /// Set the print level
    void set_print(int value) { print_ = value; }
    /// Set the maximum RDM computed (0 - 3)
    void set_max_rdm_level(int value);
    /// FCI  iterations
    void set_fci_iterations(int value);
    /// Print the NO from the 1RDM
    void print_no(bool value);
    /// Set Ms value
    void set_ms(int ms);
    /// Get the pointer of FCIWfn
    std::shared_ptr<FCIWfn> get_FCIWfn() { return fcisolver_->get_FCIWFN(); }

  private:
    // ==> Class data <==

    /// The molecular integrals
    std::shared_ptr<ForteIntegrals> ints_;
    /// The information about the molecular orbital spaces
    std::shared_ptr<MOSpaceInfo> mo_space_info_;
    /// A pointer to the FCISolver object
    std::unique_ptr<FCISolver> fcisolver_;
    /// Print level
    /// 0 : silent mode (no printing)
    /// 1 : default printing
    int print_ = 1;

    /// Set the maximum RDM computed (0 - 3)
    int max_rdm_level_;
    /// The number of iterations for FCI
    int fci_iterations_;
    /// Print the Natural Orbitals from the 1-RDM
    bool print_no_;
    /// Z component of spin times 2 (i.e. 2 * Sz)
    int ms_;
    /// Did the user set ms?
    bool set_ms_ = false;

    // ==> Class functions <==

    /// All that happens before we compute the energy
    void startup();
};

}
}

#endif // _fci_h_
