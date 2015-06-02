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

#include <numeric>
//#include <algorithm>

#include "psi4-dec.h"

#include <libmints/molecule.h>
#include <libmints/pointgrp.h>
#include "libmints/wavefunction.h"

#include "helpers.h"

namespace psi{ namespace libadaptive{


MOSpaceInfo::MOSpaceInfo()
{
    // Now we want the reference (SCF) wavefunction
    boost::shared_ptr<Wavefunction> wfn = Process::environment.wavefunction();
    nirrep_ = wfn->nirrep();
    nmopi_ = wfn->nmopi();
}

MOSpaceInfo::~MOSpaceInfo()
{

}

size_t MOSpaceInfo::size(const std::string& space)
{
    if (mo_spaces_.count(space) == 0) return 0;
    return mo_spaces_[space].first.sum();
}

Dimension MOSpaceInfo::get_dimension(const std::string& space)
{
    Dimension result(nirrep_);
    if (mo_spaces_.count(space) == 0) return result;
    return mo_spaces_[space].first;
}

std::vector<size_t> MOSpaceInfo::get_absolute_mo(const std::string& space)
{
    std::vector<size_t> result;
    if (mo_spaces_.count(space) == 0) return result;
    auto& vec_mo_info = mo_spaces_[space].second;
    for (auto& mo_info : vec_mo_info){
        result.push_back(std::get<0>(mo_info));
    }
    return result;
}

std::vector<size_t> MOSpaceInfo::get_corr_abs_mo(const std::string& space)
{
    std::vector<size_t> result;
    if (mo_spaces_.count(space) == 0) return result;
    auto& vec_mo_info = mo_spaces_[space].second;
    for (auto& mo_info : vec_mo_info){
        result.push_back(mo_to_cmo_[std::get<0>(mo_info)]);
    }
    return result;
}

std::vector<std::pair<size_t,size_t>> MOSpaceInfo::get_relative_mo(const std::string& space)
{
    std::vector<std::pair<size_t,size_t>> result;
    if (mo_spaces_.count(space) == 0) return result;
    auto& vec_mo_info = mo_spaces_[space].second;
    for (auto& mo_info : vec_mo_info){
        result.push_back(std::make_pair(std::get<1>(mo_info),std::get<2>(mo_info)));
    }
    return result;
}

void MOSpaceInfo::read_options(Options& options)
{
    outfile->Printf("\n\n  ==> MO Space Information <==\n");

    // Read the elementary spaces
    for (std::string& space : elementary_spaces_){
        std::pair<SpaceInfo,bool> result = read_mo_space(space,options);
        if (result.second){
            mo_spaces_[space] = result.first;
        }
    }

    // Handle frozen core


    // Count the assigned orbitals
    Dimension unassigned = nmopi_;
    for (auto& str_si : mo_spaces_){
        unassigned -= str_si.second.first;
    }

    for (int h = 0; h < nirrep_; ++h){
        if (unassigned[h] < 0){
            outfile->Printf("\n  There is an error in the definition of the orbital spaces.  Total unassigned MOs for irrep %d is %d.",
                            h,unassigned[h]);
        }
    }

    // Adjust size of undefined spaces
    for (std::string space : elementary_spaces_priority_){
        // Assign MOs to the undefined space with the highest priority
        if (not mo_spaces_.count(space)){
            std::vector<MOInfo> vec_mo_info;
            mo_spaces_[space] = std::make_pair(unassigned,vec_mo_info);
            for (int h = 0; h < nirrep_; ++h){ unassigned[h] = 0; }
        }
    }
    if (unassigned.sum() != 0){
        outfile->Printf("\n  There is an error in the definition of the orbital spaces.  There are %d unassigned MOs.",unassigned.sum());
        exit(1);
    }


    // Compute orbital mappings
    for (size_t h = 0, p_abs = 0; h < nirrep_; ++h){
        size_t p_rel = 0;
        for (std::string space : elementary_spaces_){
            size_t n = mo_spaces_[space].first[h];
            for (int q = 0; q < n; ++q){
                mo_spaces_[space].second.push_back(std::make_tuple(p_abs,h,p_rel));
                p_abs += 1;
                p_rel += 1;
            }
        }
    }


    // Compute the MO to correlated MO mapping
    std::vector<size_t> vec(nmopi_.sum());
    std::iota(vec.begin(),vec.end(),0);

    // Remove the frozen core/virtuals
    for (MOInfo& mo_info : mo_spaces_["FROZEN_DOCC"].second){
        outfile->Printf("\n Removing orbital %d",std::get<0>(mo_info));
        vec.erase(std::remove(vec.begin(), vec.end(),std::get<0>(mo_info)), vec.end());
    }
    for (MOInfo& mo_info : mo_spaces_["FROZEN_UOCC"].second){
        vec.erase(std::remove(vec.begin(), vec.end(),std::get<0>(mo_info)), vec.end());
    }

    mo_to_cmo_.assign(nmopi_.sum(),1000000000);
    for (size_t n = 0; n < vec.size(); ++n){
        mo_to_cmo_[vec[n]] = n;
    }

    // Define composite spaces

    // Print the space information
    size_t label_size = 1;
    for (std::string space : elementary_spaces_){
        label_size = std::max(space.size(),label_size);
    }

    int banner_width = label_size + 4 + 6 * (nirrep_ + 1);
    CharacterTable ct = Process::environment.molecule()->point_group()->char_table();
    outfile->Printf("\n  %s",std::string(banner_width,'-').c_str());
    outfile->Printf("\n    %s",std::string(label_size,' ').c_str());
    for (int h = 0; h < nirrep_; ++h) outfile->Printf(" %5s",ct.gamma(h).symbol());
    outfile->Printf("   Sum");
    outfile->Printf("\n  %s",std::string(banner_width,'-').c_str());

    for (std::string space : elementary_spaces_){
        Dimension& dim = mo_spaces_[space].first;
        outfile->Printf("\n    %-*s",label_size,space.c_str());
        for (int h = 0; h < nirrep_; ++h){
            outfile->Printf("%6d",dim[h]);
        }
        outfile->Printf("%6d",dim.sum());
    }
    outfile->Printf("\n    %-*s",label_size,"Total");
    for (int h = 0; h < nirrep_; ++h){
        outfile->Printf("%6d",nmopi_[h]);
    }
    outfile->Printf("%6d",nmopi_.sum());
    outfile->Printf("\n  %s",std::string(banner_width,'-').c_str());
}

std::pair<SpaceInfo,bool> MOSpaceInfo::read_mo_space(const std::string& space,Options& options)
{
    bool read = false;
    Dimension space_dim(nirrep_);
    std::vector<MOInfo> vec_mo_info;
    if ((options[space].has_changed()) && (options[space].size() == nirrep_)){
        for (int h = 0; h < nirrep_; ++h){
            space_dim[h] = options[space][h].to_integer();
        }
        read = true;
        outfile->Printf("\n  Read options for space %s",space.c_str());
    }else{
//        outfile->Printf("\n  The size of space \"%s\" (%d) does not match the number of irreducible representations (%zu).",
//                        space.c_str(),options[space].size(),nirrep_);
    }
    SpaceInfo space_info(space_dim,vec_mo_info);
    return std::make_pair(space_info,read);
}


void print_method_banner(const std::vector<std::string>& text, const std::string &separator)
{
    size_t max_width = 80;

    size_t width = 0;
    for (auto& line : text){
        width = std::max(width,line.size());
    }

    std::string tab((max_width - width - 4)/2,' ');
    std::string header(width + 4,char(separator[0]));

    *outfile << "\n\n" << tab << header << std::endl;
    for (auto& line : text){
        size_t padding = 2 + (width - line.size()) / 2;
        *outfile << tab << std::string(padding,' ') << line << std::endl;
    }
    *outfile << tab << header << std::endl;

    outfile->Flush();
}


void print_h2(const std::string& text, const std::string& left_separator, const std::string& right_separator)
{
    outfile->Printf("\n\n  %s %s %s\n",left_separator.c_str(),
                    text.c_str(),right_separator.c_str());
}

Matrix tensor_to_matrix(ambit::Tensor t,Dimension dims)
{
    // Copy the tensor to a plain matrix
    size_t size = dims.sum();
    Matrix M("M",size,size);
    t.iterate([&](const std::vector<size_t>& i,double& value){
        M.set(i[0],i[1],value);
    });

    Matrix M_sym("M",dims,dims);
    size_t offset = 0;
    for (size_t h = 0; h < dims.n(); ++h){
        for (size_t p = 0; p < dims[h]; ++p){
            for (size_t q = 0; q < dims[h]; ++q){
                double value = M.get(p + offset,q + offset);
                M_sym.set(h,p,q,value);
            }
        }
        offset += dims[h];
    }
    return M_sym;
}

}} // End Namespaces
