/* Copyright 2012 Tobias Marschall
 * 
 * This file is part of HaploClique.
 * 
 * HaploClique is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HaploClique is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HaploClique.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <vector>
#include <algorithm>
#include <math.h>
#include <ctype.h>
#include <map>
#include <array>

#include <boost/tokenizer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/compare.hpp>

#include "AlignmentRecord.h"
#include "Clique.h"

using namespace std;
using namespace boost;

namespace {
//helper functions to merge DNA sequences
int agreement(const char& qual1, const char& qual2){
    float prob1 = std::pow(10,(float)-qual1/10);
    float prob2 = std::pow(10,(float)-qual2/10);
    float posterior = (prob1*prob2/3)/(1-prob1-prob2+4*prob1*prob2/3);
    posterior = std::round(-10*log10(posterior));
    return posterior;
}

int disagreement(const char& qual1, const char& qual2){
    float prob1 = std::pow(10,(float)-qual1/10);
    float prob2 = std::pow(10,(float)-qual2/10);
    float posterior = ((prob1*(1-prob2/3))/(prob1+prob2-4*prob1*prob2/3));
    posterior = std::round(-10*log10(posterior));
    return posterior;
}

float phredProb(const char& qual){
    return std::pow(10, (double)(-qual)/10.0);
}
//error probabilites are precomputed for all phred scores
std::array<float, 127> compute_error_probs(){
    std::array<float, 127> result;
    for (int i = 33; i < result.size(); i++){
        result[i] = phredProb(i-33);
    }
    return result;
}
//new values for updated error probabilites are precomputed
std::array<std::array<int, 127>, 127> compute_error_agreement(){
    std::array<std::array<int, 127>, 127> result;
    for (int i = 33; i < result.size(); i++){
        for(int j = 33; j < result.size(); j++){
            result[i][j] = agreement(i-33,j-33);
        }
    }
    return result;
}

std::array<std::array<int, 127>, 127> compute_error_disagreement(){
    std::array<std::array<int, 127>, 127> result;
    for (int i = 33; i < result.size(); i++){
        for(int j = 33; j < result.size(); j++){
            result[i][j] = disagreement(i-33,j-33);
        }
    }
    return result;
}

std::array<float, 127> error_probs = compute_error_probs();
std::array<std::array<int, 127>, 127> error_agreement = compute_error_agreement();
std::array<std::array<int, 127>, 127> error_disagreement = compute_error_disagreement();
}

int phred_sum(const string& phred, char phred_base=33) {
	int result = 0;
	for (size_t i=0; i<phred.size(); ++i) {
		result += phred[i] - phred_base;
	}
	return result;
}

//called by getmergedDnaSequence() to create new CigarData
std::vector<BamTools::CigarOp> createCigar(std::string& nucigar){
    std::vector<BamTools::CigarOp> res;
    unsigned int counter = 1;
    unsigned int pos = 0;
    char c;
    while(pos < nucigar.size()){
        c = nucigar[pos];
        pos++;
        while(pos < nucigar.size()){
            if (c == nucigar[pos]){
                counter++;
                pos++;
            } else {
                res.push_back(BamTools::CigarOp(c,counter));
                counter = 1;
                break;
            }
        }
        if(pos == nucigar.size()){
            res.push_back(BamTools::CigarOp(c,counter));
        }
    }
    return res;
}

int computeOffset(const std::vector<char>& cigar){
    int offset = 0;
    for(auto& i : cigar){
        if (i == 'S' || i == 'H'){
            offset++;
        } else break;
    }
    return offset;
}

void computeSOffset(const std::vector<char>& cigar,int& c, int& q){
    for(auto& i : cigar){
        if (i == 'S'){
            c++;
            q++;
        } else if(i == 'H'){
            c++;
        } else break;
    }
}

int computeRevOffset(const std::vector<char>& cigar){
    std::vector<char> t = cigar;
    int offset = 0;
    for (std::vector<char>::reverse_iterator it = t.rbegin(); it != t.rend(); ++it){
        if (*it == 'S' || *it == 'H'){
            offset++;
        } else break;
    }
    return offset;
}

int computeRevSOffset(const std::vector<char>& cigar){
    std::vector<char> t = cigar;
    int offset = 0;
    for (std::vector<char>::reverse_iterator it = t.rbegin(); it != t.rend(); ++it){
        if (*it == 'S'){
            offset++;
        } else break;
    }
    return offset;
}

std::pair<char,char> computeEntry(const char& base1, const char& qual1, const char& base2, const char& qual2){
    std::pair<char,char> result;

    if (base1==base2){
        result.first = base1;
        result.second = std::min(error_agreement[qual1][qual2]+33,126);
    }
    else if (qual1>=qual2) {
        result.first = base1;
        result.second = error_disagreement[qual1][qual2]+33;
    } else {
        result.first = base2;
        result.second = error_disagreement[qual2][qual1]+33;
    }
    return result;
}


AlignmentRecord::AlignmentRecord(const BamTools::BamAlignment& alignment, int readRef, vector<string>* rnm) : readNameMap(rnm) {
    this->single_end = true;
    this->readNames.insert(readRef);
    this->name = alignment.Name;
    this->start1 = alignment.Position + 1;
    this->end1 = alignment.GetEndPosition();
    this->cigar1 = alignment.CigarData;
    this->sequence1 = ShortDnaSequence(alignment.QueryBases, alignment.Qualities);
    this->phred_sum1 = phred_sum(alignment.Qualities);
    this->length_incl_deletions1 = this->sequence1.size();
    this->length_incl_longdeletions1 = this->sequence1.size();
	for (const auto& it : cigar1) {
        for (unsigned int s = 0; s < it.Length; ++s) {
          	this->cigar1_unrolled.push_back(it.Type);
        }
        if (it.Type == 'D') {
      		this->length_incl_deletions1+=it.Length;
       		if (it.Length > 1) {
       			this->length_incl_longdeletions1+=it.Length;
       		}
        }/*for Debugging
        if (it.Type == 'H'){
            int k = 0;
        }
        if (it.Type == 'N'){
            cout << alignment.Name << endl;
        }
        if (it.Type == 'P'){
           cout << alignment.Name << endl;
        }
        if (it.Type == 'I'){
            int k = 0;
        }*/
    }
    this->cov_pos = this->coveredPositions();
}

AlignmentRecord::AlignmentRecord(unique_ptr<vector<const AlignmentRecord*>>& alignments, unsigned int clique_id) : cigar1_unrolled(), cigar2_unrolled() {
    //no longer majority vote, phred scores are updated according to Edgar et al.
    assert ((*alignments).size()>1);
    //get first AlignmentRecord
    auto& al1 = (*alignments)[0];
    //auto& al1 = (*alignments)[(*alignments).size()-1];
    this->start1 = al1->getStart1();
    this->end1 = al1->getEnd1();
    this->cigar1 = al1->getCigar1();
    this->cigar1_unrolled = al1->getCigar1Unrolled();
    this->sequence1 = al1->getSequence1();
    this->readNameMap = al1->readNameMap;
    this->readNames.insert(al1->readNames.begin(), al1->readNames.end());
    this->single_end = al1->isSingleEnd();

    if(al1->isPairedEnd()){
        this->start2 = al1->getStart2();
        this->end2 = al1->getEnd2();
        this->cigar2 = al1->getCigar2();
        this->cigar2_unrolled = al1->getCigar2Unrolled();
        this->sequence2 = al1->getSequence2();
    }    //merge recent AlignmentRecord with all other alignments of Clique
    for (unsigned int i = 1; i < (*alignments).size(); i++){
    //for (int i = (*alignments).size()-2; i >= 0; i--){
        auto& al = (*alignments)[i];
        if (this->single_end && al->isSingleEnd()){
            mergeAlignmentRecordsSingle(*al,1,1);
        }
        else if (!(this->single_end) && al->isPairedEnd()){
            //cout << "Clique with Paired Ends merged" << endl;
            mergeAlignmentRecordsPaired(*al);
        }
        else {
            //cout << "Clique with Mixed Ends merged" << endl;
            mergeAlignmentRecordsMixed(*al);
        }

        this->readNames.insert(al->readNames.begin(),al->readNames.end());

        //if(this->isPairedEnd() && this->end2 - this->start1 > 5000){
        //   std::vector<std::string> names = this->getReadNames();
        //    for (auto& i : names){
        //        cout << i << endl;
        //    }
        //}
    }
    //update name of new Clique Superread
    this->name = "Clique_" + to_string(clique_id);
    /*
    unsigned int length_ct = 0; // DEBUG
    for (const auto& it : cigar1) { //DEBUG
        length_ct += it.Length;
        if (it.Type == 'D') {
            this->length_incl_deletions1+=it.Length;
            if (it.Length > 1) {
                this->length_incl_longdeletions1+=it.Length;
            }
        }
    }
    assert(length_ct == (unsigned)this->length_incl_deletions1); //DEBUG


    if(not this->single_end) {
        this->length_incl_deletions2 = this->sequence2.size();
        this->length_incl_longdeletions2 = this->sequence2.size();

        length_ct = 0; //DEBUG
        for (const auto& it : cigar2) {
            length_ct += it.Length; //DEBUG
            if (it.Type == 'D') {
                this->length_incl_deletions2+=it.Length;
                if (it.Length > 1) {
                    this->length_incl_longdeletions2+=it.Length;
                }
            }
        }
        assert(length_ct == (unsigned)this->length_incl_deletions2); //DEBUG
    } */
    this->cov_pos=this->coveredPositions();
}

//combines reads to paired end reads (single end given overlapping paired ends) when readBamFile is run
void AlignmentRecord::pairWith(const BamTools::BamAlignment& alignment) {
    if ((unsigned)(alignment.Position+1) > this->end1) {
        this->single_end = false;
        this->start2 = alignment.Position + 1;
        this->end2 = alignment.GetEndPosition();
        if (!(this->end2 > 0)){
            cout << this->name << " end: " << this->end2 << endl;
        }
        this->cigar2 = alignment.CigarData;
        this->sequence2 = ShortDnaSequence(alignment.QueryBases, alignment.Qualities);
        this->phred_sum2 = phred_sum(alignment.Qualities);

        this->length_incl_deletions2 = this->sequence2.size();
        this->length_incl_longdeletions2 = this->sequence2.size();
        for (const auto& it : cigar2) {
            for (unsigned int s = 0; s < it.Length; ++s) {
                this->cigar2_unrolled.push_back(it.Type);
            }
            if (it.Type == 'D') {
                this->length_incl_deletions2+=it.Length;
                if (it.Length > 1) {
                    this->length_incl_longdeletions2+=it.Length;
                }
            }
        }
        this->cov_pos = this->coveredPositions();
    } else if ((unsigned)alignment.GetEndPosition() < this->start1) {
        this->single_end = false;
        this->start2 = this->start1;
        this->end2 = this->end1;
        this->cigar2 = this->cigar1;
        this->sequence2 = this->sequence1;
        this->phred_sum2 = this->phred_sum1;
        this->cigar2_unrolled = this->cigar1_unrolled;
        this->length_incl_deletions2 = this->length_incl_deletions1;
        this->length_incl_longdeletions2 = this->length_incl_longdeletions1;

        this->start1 = alignment.Position + 1;
        this->end1 = alignment.GetEndPosition();
        //if (!(this->end1 > 0)){
        //    cout << this->name << " end: " << this->end1 << endl;
        //}
        this->cigar1 = alignment.CigarData;
        this->sequence1 = ShortDnaSequence(alignment.QueryBases, alignment.Qualities);
        this->phred_sum1 = phred_sum(alignment.Qualities);
        this->length_incl_deletions1 = this->sequence1.size();
        this->length_incl_longdeletions1 = this->sequence1.size();
        this->cigar1_unrolled.clear();
        for (const auto& it : cigar1) {
            for (unsigned int s = 0; s < it.Length; ++s) {
                this->cigar1_unrolled.push_back(it.Type);
            }
            if (it.Type == 'D') {
                this->length_incl_deletions1+=it.Length;
                if (it.Length > 1) {
                    this->length_incl_longdeletions1+=it.Length;
                }
            }
        }
        this->cov_pos = this->coveredPositions();
    }//merging of overlapping paired ends to single end reads
    else {
        this->getMergedDnaSequence(alignment);
    }
}

//computes vector for AlignmentRecord which contains information about the mapping position in the reference, the base and its phred score, the error probability, the position of the base in the original read and an annotation in which read the base occurs given paired end reads.
std::vector<AlignmentRecord::mapValue> AlignmentRecord::coveredPositions() const{
    std::vector<AlignmentRecord::mapValue> cov_positions;
    //position in ref
    int r = this->start1;
    //position in querybases / quality string of read
    int q = 0;
    for (unsigned int i = 0; i< this->cigar1_unrolled.size(); ++i){
        char c = this->cigar1_unrolled[i];
        switch(c){
            case 'M': {
                c = this->sequence1[q];
                char qual = this->sequence1.qualityChar(q);
                cov_positions.push_back({r,c,qual,error_probs[qual],q,0});
                //char d = this->sequence1[q];
                ++q;
                ++r;
                break;
            }
            case 'D': {
                ++r;
                break;
            }
            case 'S':
            case 'I': {
                ++q;
                break;
            }
            case 'H':
                break;
        }
    }
    //In case of a paired end read
    if (!this->single_end){
        assert(this->start1 <= this->start2);
            //position in ref
            r = this->start2;
            //position in query bases
            q = 0;
            //in case of overlapping paired end it is declared as a single end
            //if (r <= this->end1){
            //   this->single_end = true;
            //}
            for (unsigned int i = 0; i< this->cigar2_unrolled.size(); ++i){
                char c = this->cigar2_unrolled[i];
                switch(c){
                    case 'M': {
                        c = this->sequence2[q];
                        char qual = this->sequence2.qualityChar(q);
                        cov_positions.push_back({r,c,qual,error_probs[qual],q,1});
                        ++q;
                        ++r;
                        break;
                    }
                    case 'D': {
                        ++r;
                        break;
                    }
                    case 'S':
                    case 'I': {
                        ++q;
                        break;
                    }
                    case 'H':
                        break;
              }
           }
        }
    return cov_positions;
}

//creates merged DNA sequences and Cigar string out of overlapping paired end reads while reading in BAM Files
void AlignmentRecord::getMergedDnaSequence(const BamTools::BamAlignment& alignment){
        std::string dna = "";
        std::string qualities = "";
        std::string nucigar = "";
        std::vector<char> cigar_temp_unrolled;
        //vector of CigarOp
        for (const auto& it : alignment.CigarData) {
            for (unsigned int s = 0; s < it.Length; ++s) {
                cigar_temp_unrolled.push_back(it.Type);
            }
        }
        //get starting position and ending position according to ref position, paying attention to clipped bases
        int offset_f1 = computeOffset(this->cigar1_unrolled);
        int offset_f2 = computeOffset(cigar_temp_unrolled);
        int offset_b1 = computeRevOffset(this->cigar1_unrolled);
        int offset_b2 = computeRevOffset(cigar_temp_unrolled);

        //updated ref position including clips
        int ref_s_pos1 = this->start1-offset_f1;
        int ref_e_pos1 = this->end1+offset_b1;
        int ref_s_pos2 = alignment.Position+1-offset_f2;
        int ref_e_pos2 = alignment.GetEndPosition()+offset_b2;
        //position in query sequences // phred scores
        int q_pos1 = 0;
        int q_pos2 = 0;
        //position in unrolled cigar vectors
        int c_pos1 = 0;
        int c_pos2 = 0;
        //4 cases of different overlaps
        //------------
        //     ------------
        if(ref_s_pos1 <= ref_s_pos2 && ref_e_pos1 <= ref_e_pos2){
            while(ref_s_pos1<ref_s_pos2){
                noOverlapMerge(dna,qualities,nucigar,c_pos1,q_pos1,ref_s_pos1,1);
            }
            while(ref_s_pos1<=ref_e_pos1){
                overlapMerge(alignment,dna,qualities,nucigar,cigar_temp_unrolled,c_pos1,c_pos2,q_pos1,q_pos2,ref_s_pos1);
            }
            while(ref_s_pos1<=ref_e_pos2){
                noOverlapMerge(alignment, dna, qualities, nucigar, cigar_temp_unrolled, c_pos2, q_pos2, ref_s_pos1);
            }
        }//------------------------------
            //           ----------
         else if (ref_s_pos1 <= ref_s_pos2 && ref_e_pos1 >= ref_e_pos2){
            while(ref_s_pos1<ref_s_pos2){
                noOverlapMerge(dna,qualities,nucigar,c_pos1,q_pos1,ref_s_pos1,1);
            }
            while(ref_s_pos1<=ref_e_pos2){
                overlapMerge(alignment,dna,qualities,nucigar,cigar_temp_unrolled,c_pos1,c_pos2,q_pos1,q_pos2,ref_s_pos1);
            }
            while(ref_s_pos1<=ref_e_pos1){
                noOverlapMerge(dna,qualities,nucigar,c_pos1,q_pos1,ref_s_pos1,1);
            }
            //         ----------
            //--------------------------
        } else if (ref_s_pos1 >= ref_s_pos2 && ref_e_pos1 <= ref_e_pos2){
            while(ref_s_pos2<ref_s_pos1){
                noOverlapMerge(alignment, dna, qualities, nucigar, cigar_temp_unrolled, c_pos2, q_pos2, ref_s_pos2);
            }
            while(ref_s_pos2<=ref_e_pos1){
                overlapMerge(alignment,dna,qualities,nucigar,cigar_temp_unrolled,c_pos1,c_pos2,q_pos1,q_pos2,ref_s_pos2);
            }
            while(ref_s_pos2<=ref_e_pos2){
                noOverlapMerge(alignment, dna, qualities, nucigar, cigar_temp_unrolled, c_pos2, q_pos2, ref_s_pos2);
            }
            //           --------------------
            //---------------------
        } else {
            assert(ref_s_pos1 >= ref_s_pos2 && ref_e_pos1 >= ref_e_pos2);
            while(ref_s_pos2<ref_s_pos1){
                noOverlapMerge(alignment, dna, qualities, nucigar, cigar_temp_unrolled, c_pos2, q_pos2, ref_s_pos2);
            }
            while(ref_s_pos2<=ref_e_pos2){
                overlapMerge(alignment,dna,qualities,nucigar,cigar_temp_unrolled,c_pos1,c_pos2,q_pos1,q_pos2,ref_s_pos2);
            }
            while(ref_s_pos2<=ref_e_pos1){
                noOverlapMerge(dna,qualities,nucigar,c_pos1,q_pos1,ref_s_pos2,1);
            }
        }
        this->start1 = std::min(this->start1,(unsigned int)alignment.Position+1);
        this->end1=std::max((unsigned int)alignment.GetEndPosition(),this->end1);
        this->single_end= true;
        this->cigar1 = createCigar(nucigar);
        this->sequence1=ShortDnaSequence(dna,qualities);
        this->phred_sum1=phred_sum(qualities);
        this->length_incl_deletions1 = this->sequence1.size();
        this->length_incl_longdeletions1 = this->sequence1.size();
        this->cigar1_unrolled.clear();
        for (char i : nucigar){
            this->cigar1_unrolled.push_back(i);
        }
        this->cov_pos = this->coveredPositions();
}

//helper function for getMergedDnaSequence
void AlignmentRecord::noOverlapMerge(const BamTools::BamAlignment& alignment, std::string& dna, std::string& qualities, std::string& nucigar, std::vector<char>& cigar_temp_unrolled, int& c_pos, int& q_pos, int& ref_pos) const{
    char c = cigar_temp_unrolled[c_pos];
    if (c == 'H'){
        ref_pos++;
        c_pos++;
    } else if (c == 'I') {
        dna += alignment.QueryBases[q_pos];
        qualities += alignment.Qualities[q_pos];
        nucigar += 'I';
        q_pos++;
        c_pos++;
    } else if (c == 'D') {
        nucigar += 'D';
        ref_pos++;
        c_pos++;
    } else if (c == 'S'){
        ref_pos++;
        q_pos++;
        c_pos++;
    } else if (c == 'M'){
        dna += alignment.QueryBases[q_pos];
        qualities += alignment.Qualities[q_pos];
        nucigar += c;
        ref_pos++;
        q_pos++;
        c_pos++;
    } else {
        assert(false);
    }
}

//helper function for getMergedDnaSequence, clipped bases are NOT contained in final sequence
void AlignmentRecord::overlapMerge(const BamTools::BamAlignment& alignment, std::string& dna, std::string& qualities, std::string& nucigar, std::vector<char>& cigar_temp_unrolled, int& c_pos1, int& c_pos2, int& q_pos1, int& q_pos2, int& ref_pos) const{
    char c1 = this->cigar1_unrolled[c_pos1];
    char c2 = cigar_temp_unrolled[c_pos2];
    if((c1 == 'M' && c2 == 'M') || (c1 == 'S' && c2 == 'S') || (c1 == 'I' && c2 == 'I')){
        if (c1 != 'S'){
            std::pair<char,char> resPair = computeEntry(this->sequence1[q_pos1],this->sequence1.qualityChar(q_pos1),alignment.QueryBases[q_pos2],alignment.Qualities[q_pos2]);
            dna += resPair.first;
            qualities += resPair.second;
            nucigar += c1;
        }
        if (c1 != 'I') ref_pos++;
        q_pos1++;
        q_pos2++;
        c_pos1++;
        c_pos2++;
    } else if ((c1 == 'D' && c2 == 'D') || (c1 == 'H' && c2 == 'H') || (c1 == 'D' && c2 == 'H') || (c1 == 'H' && c2 == 'D') || (c1 == 'D' && c2 == 'S') || (c1 == 'S' && c2 == 'D')){
        c_pos1++;
        c_pos2++;
        ref_pos++;
        if (c1 == 'D' || c2 == 'D'){
            nucigar += 'D';
        }
        if (c1 == 'S'){
            q_pos1++;
        } else if(c2 == 'S'){
            q_pos2++;
        }
    } else if ((c1 == 'M' && (c2 == 'D' || c2 == 'H' || c2 == 'S')) || ((c1 == 'D' || c1 == 'H' || c1 == 'S') && c2 == 'M') || (c1 == 'S' && c2 == 'H') || (c1 == 'H' && c2 == 'S')) {
        if (c1 == 'M'){
            nucigar += 'M';
            dna += this->sequence1[q_pos1];
            qualities += this->sequence1.qualityChar(q_pos1);
            ref_pos++;
            c_pos1++;
            c_pos2++;
            q_pos1++;
            if (c2 == 'S') q_pos2++;
        } else if (c2 == 'M'){
            nucigar += 'M';
            dna +=  alignment.QueryBases[q_pos2];
            qualities += alignment.Qualities[q_pos2];
            ref_pos++;
            c_pos1++;
            c_pos2++;
            q_pos2++;
            if (c1 == 'S') q_pos1++;
        } else if (c1 == 'S'){
            ref_pos++;
            c_pos1++;
            c_pos2++;
            q_pos1++;
        } else {
            ref_pos++;
            c_pos1++;
            c_pos2++;
            q_pos2++;
        }
    } else if (c1 == 'I' || c2 == 'I'){
        if(c1 == 'I'){
            nucigar += 'I';
            dna += this->sequence1[q_pos1];
            qualities += this->sequence1.qualityChar(q_pos1);
            c_pos1++;
            q_pos1++;
        } else {
            nucigar += 'I';
            dna +=  alignment.QueryBases[q_pos2];
            qualities += alignment.Qualities[q_pos2];
            c_pos2++;
            q_pos2++;
        }
    } else {
        assert(false);
    }
}

//method is partly also used by mergeAlignmentRecordsMixed and mergeAlignmentRecordsPaired, i = ith cigar of this, j = ith cigar of AlignmentRecord ar
void AlignmentRecord::mergeAlignmentRecordsSingle(const AlignmentRecord& ar, int i, int j){
    std::string dna = "";
    std::string qualities = "";
    std::string nucigar = "";
    int offset_f1, offset_f2, offset_b1, offset_b2, ref_s_pos1, ref_e_pos1, ref_s_pos2, ref_e_pos2 = 0;
    //i and j determine which cigar strings / sequences are considered
    if (i == 1){
         //get starting position and ending position according to ref position, paying attention to clipped bases
         //updated ref position including clips
         offset_f1 = computeOffset(this->cigar1_unrolled);
         offset_b1 = computeRevOffset(this->cigar1_unrolled);
         ref_e_pos1 = this->end1+offset_b1;
         ref_s_pos1 = this->start1-offset_f1;
         if(j == 1){
             std::vector<char> cigar = ar.getCigar1Unrolled();
             offset_f2 = computeOffset(cigar);
             offset_b2 = computeRevOffset(cigar);
             ref_s_pos2 = ar.getStart1()-offset_f2;
             ref_e_pos2 = ar.getEnd1()+offset_b2;
         } else {
             std::vector<char> cigar = ar.getCigar2Unrolled();
             offset_f2 = computeOffset(cigar);
             offset_b2 = computeRevOffset(cigar);
             ref_s_pos2 = ar.getStart2()-offset_f2;
             ref_e_pos2 = ar.getEnd2()+offset_b2;
         }
    } else {
         //get starting position and ending position according to ref position, paying attention to clipped bases
         //updated ref position including clips
         offset_f1 = computeOffset(this->cigar2_unrolled);
         offset_b1 = computeRevOffset(this->cigar2_unrolled);
         ref_s_pos1 = this->start2-offset_f1;
         ref_e_pos1 = this->end2+offset_b1;
         if(j == 1){
             std::vector<char> cigar = ar.getCigar1Unrolled();
             offset_f2 = computeOffset(cigar);
             offset_b2 = computeRevOffset(cigar);
             ref_s_pos2 = ar.getStart1()-offset_f2;
             ref_e_pos2 = ar.getEnd1()+offset_b2;
         } else {
             std::vector<char> cigar = ar.getCigar2Unrolled();
             offset_f2 = computeOffset(cigar);
             offset_b2 = computeRevOffset(cigar);
             ref_s_pos2 = ar.getStart2()-offset_f2;
             ref_e_pos2 = ar.getEnd2()+offset_b2;
         }
    }

    //position in query sequences // phred scores
    int q_pos1 = 0;
    int q_pos2 = 0;
    //position in unrolled cigar vectors
    int c_pos1 = 0;
    int c_pos2 = 0;
    //4 cases of different overlaps
    //------------
    //     ------------
    if(ref_s_pos1 <= ref_s_pos2 && ref_e_pos1 <= ref_e_pos2){
        while(ref_s_pos1<ref_s_pos2){
            noOverlapMerge(dna,qualities,nucigar,c_pos1,q_pos1,ref_s_pos1,i);
        }
        while(ref_s_pos1<=ref_e_pos1){
            overlapMerge(ar,dna,qualities,nucigar,c_pos1,c_pos2,q_pos1,q_pos2,ref_s_pos1,i,j);
        }
        while(ref_s_pos1<=ref_e_pos2){
            ar.noOverlapMerge(dna, qualities, nucigar, c_pos2, q_pos2, ref_s_pos1,j);
        }
    }//------------------------------
        //           ----------
     else if (ref_s_pos1 <= ref_s_pos2 && ref_e_pos1 >= ref_e_pos2){
        while(ref_s_pos1<ref_s_pos2){
            noOverlapMerge(dna,qualities,nucigar,c_pos1,q_pos1,ref_s_pos1,i);
        }
        while(ref_s_pos1<=ref_e_pos2){
            overlapMerge(ar,dna,qualities,nucigar,c_pos1,c_pos2,q_pos1,q_pos2,ref_s_pos1,i,j);
        }
        while(ref_s_pos1<=ref_e_pos1){
            noOverlapMerge(dna,qualities,nucigar,c_pos1,q_pos1,ref_s_pos1,i);
        }
        //         ----------
        //--------------------------
    } else if (ref_s_pos1 >= ref_s_pos2 && ref_e_pos1 <= ref_e_pos2){
        while(ref_s_pos2<ref_s_pos1){
            ar.noOverlapMerge(dna, qualities, nucigar, c_pos2, q_pos2, ref_s_pos2,j);
        }
        while(ref_s_pos2<=ref_e_pos1){
            overlapMerge(ar,dna,qualities,nucigar,c_pos1,c_pos2,q_pos1,q_pos2,ref_s_pos2,i,j);
        }
        while(ref_s_pos2<=ref_e_pos2){
            ar.noOverlapMerge(dna, qualities, nucigar, c_pos2, q_pos2, ref_s_pos2,j);
        }
        //           --------------------
        //---------------------
    } else {
        assert(ref_s_pos1 >= ref_s_pos2 && ref_e_pos1 >= ref_e_pos2);
        while(ref_s_pos2<ref_s_pos1){
            ar.noOverlapMerge(dna, qualities, nucigar, c_pos2, q_pos2, ref_s_pos2,j);
        }
        while(ref_s_pos2<=ref_e_pos2){
            overlapMerge(ar,dna,qualities,nucigar, c_pos1,c_pos2,q_pos1,q_pos2,ref_s_pos2,i,j);
        }
        while(ref_s_pos2<=ref_e_pos1){
            noOverlapMerge(dna,qualities,nucigar, c_pos1,q_pos1,ref_s_pos2,i);
        }
    }
    if (i == 1){
        if(j == 1){
            this->start1 = std::min(this->start1,ar.getStart1());
            this->end1=std::max(ar.getEnd1(),this->end1);
            this->single_end= true;
            this->cigar1 = createCigar(nucigar);
            this->sequence1=ShortDnaSequence(dna,qualities);
            this->phred_sum1=phred_sum(qualities);
            this->length_incl_deletions1 = this->sequence1.size();
            this->length_incl_longdeletions1 = this->sequence1.size();
            this->cigar1_unrolled.clear();
            for (char i : nucigar){
                this->cigar1_unrolled.push_back(i);
            }
        } else {
            this->start2 = std::min(this->start1,ar.getStart2());
            this->end2 = std::max(this->end1,ar.getEnd2());
            this->cigar2 = createCigar(nucigar);
            this->sequence2=ShortDnaSequence(dna,qualities);
            this->phred_sum2=phred_sum(qualities);
            this->length_incl_deletions2 = this->sequence2.size();
            this->length_incl_longdeletions2 = this->sequence2.size();
            this->cigar2_unrolled.clear();
            for (char i : nucigar){
                this->cigar2_unrolled.push_back(i);
            }
        }
    } else {
        if(j == 1){
            this->start2 = std::min(this->start2,ar.getStart1());
            this->end2=std::max(ar.getEnd1(),this->end2);
        } else {
            this->start2 = std::min(this->start2,ar.getStart2());
            this->end2=std::max(ar.getEnd2(),this->end2);
        }
        this->single_end= false;
        this->cigar2 = createCigar(nucigar);
        this->sequence2=ShortDnaSequence(dna,qualities);
        this->phred_sum2=phred_sum(qualities);
        this->length_incl_deletions2 = this->sequence2.size();
        this->length_incl_longdeletions2 = this->sequence2.size();
        this->cigar2_unrolled.clear();
        for (char i : nucigar){
            this->cigar2_unrolled.push_back(i);
        }
    }
}

void AlignmentRecord::mergeAlignmentRecordsPaired(const AlignmentRecord& ar){
    std::string dna = "";
    std::string qualities = "";
    std::string nucigar = "";
    //get starting position and ending position according to ref position, paying attention to clipped bases
    int offset_f1_c1 = computeOffset(this->cigar1_unrolled);
    int offset_f1_c2 = computeOffset(this->cigar2_unrolled);
    int offset_f2_c1 = computeOffset(ar.getCigar1Unrolled());
    int offset_f2_c2 = computeOffset(ar.getCigar2Unrolled());
    int offset_b1_c1 = computeRevOffset(this->cigar1_unrolled);
    int offset_b1_c2 = computeRevOffset(this->cigar2_unrolled);
    int offset_b2_c1 = computeRevOffset(ar.getCigar1Unrolled());
    int offset_b2_c2 = computeRevOffset(ar.getCigar2Unrolled());
    //updated ref position including clips
    int ref_s_pos1_c1 = this->start1-offset_f1_c1;
    int ref_e_pos1_c1 = this->end1+offset_b1_c1;
    int ref_s_pos1_c2 = this->start2-offset_f1_c2;
    int ref_e_pos1_c2 = this->end2+offset_b1_c2;
    int ref_s_pos2_c1 = ar.getStart1()-offset_f2_c1;
    int ref_e_pos2_c1 = ar.getEnd1()+offset_b2_c1;
    int ref_s_pos2_c2 = ar.getStart2()-offset_f2_c2;
    int ref_e_pos2_c2 = ar.getEnd2()+offset_b2_c2;
    //position in query sequences // phred scores
    int q_c1_pos1 = 0;
    int q_c2_pos1 = 0;
    int q_c1_pos2 = 0;
    int q_c2_pos2 = 0;
    //position in unrolled cigar vectors
    int c_c1_pos1 = 0;
    int c_c2_pos1 = 0;
    int c_c1_pos2 = 0;
    int c_c2_pos2 = 0;
    //int i;
    // --------    |  -----------    <-this
    //   --------  |     ----------
    if(this->end1 < ar.getStart2() && this->start2 > ar.getEnd1()){
        mergeAlignmentRecordsSingle(ar,1,1);
        mergeAlignmentRecordsSingle(ar,2,2);
    }//----------    ------------   <-this
    //    ---------------   -----------
    else if(ref_s_pos1_c1 <= ref_s_pos2_c1 && this->end1 >= ar.getStart1() && this->start2 <= ar.getEnd1() && this->end2 >= ar.getStart2()){
        while(ref_s_pos1_c1 < ref_s_pos2_c1){
            noOverlapMerge(dna,qualities,nucigar,c_c1_pos1,q_c1_pos1,ref_s_pos1_c1,1);
        }
        while(ref_s_pos1_c1<=this->end1){
            overlapMerge(ar,dna,qualities,nucigar,c_c1_pos1,c_c1_pos2,q_c1_pos1,q_c1_pos2,ref_s_pos1_c1,1,1);
        }
        while(ref_s_pos1_c1<this->start2){
            ar.noOverlapMerge(dna,qualities,nucigar,c_c1_pos2,q_c1_pos2,ref_s_pos1_c1,1);
        }
        computeSOffset(this->cigar2_unrolled,c_c2_pos1,q_c2_pos1);
        while(ref_s_pos1_c1<=ar.getEnd1()){
            overlapMerge(ar,dna,qualities,nucigar,c_c2_pos1,c_c1_pos2,q_c2_pos1,q_c1_pos2,ref_s_pos1_c1,2,1);
        }
        while(ref_s_pos1_c1<ar.getStart2()){
            noOverlapMerge(dna,qualities,nucigar,c_c2_pos1,q_c2_pos1,ref_s_pos1_c1,2);
        }
        computeSOffset(ar.getCigar2Unrolled(),c_c2_pos2,q_c2_pos2);
        while(ref_s_pos1_c1<=ref_e_pos2_c2 && ref_s_pos1_c1 <= ref_e_pos1_c2){
            overlapMerge(ar,dna,qualities,nucigar,c_c2_pos1,c_c2_pos2,q_c2_pos1,q_c2_pos2,ref_s_pos1_c1,2,2);
        }
        if(ref_s_pos1_c1-1 == ref_e_pos2_c2){
            while(ref_s_pos1_c1<=ref_e_pos1_c2){
                noOverlapMerge(dna,qualities,nucigar,c_c2_pos1,q_c2_pos1,ref_s_pos1_c1,2);
            }
        } else if (ref_s_pos1_c1-1 == ref_e_pos1_c2){
            while(ref_s_pos1_c1<=ref_e_pos2_c2){
                ar.noOverlapMerge(dna,qualities,nucigar,c_c2_pos2,q_c2_pos2,ref_s_pos1_c1,2);
            }
        }
        this->start1=std::min(this->start1,ar.getStart1());
        this->end1=std::max(ar.getEnd2(),this->end2);
        this->single_end = true;
        this->cigar1=createCigar(nucigar);
        this->sequence1=ShortDnaSequence(dna,qualities);
        this->phred_sum1=phred_sum(qualities);
        this->length_incl_deletions1 = this->sequence1.size();
        this->length_incl_longdeletions1 = this->sequence1.size();
        this->cigar1_unrolled.clear();
        for (char i : nucigar){
            this->cigar1_unrolled.push_back(i);
        }
   } //-----------       ----------- <-this
    //    -----------------               -----------
    else if(ref_s_pos1_c1 <= ref_s_pos2_c1 && this->end1 >=ar.getStart1() && this->start2 <= ar.getEnd1() && this->end2 < ar.getStart2()){
        while(ref_s_pos1_c1 <= ref_s_pos2_c1){
            noOverlapMerge(dna,qualities,nucigar,c_c1_pos1,q_c1_pos1,ref_s_pos1_c1,1);
        }
        while(ref_s_pos1_c1<=this->end1){
            overlapMerge(ar,dna,qualities,nucigar,c_c1_pos1,c_c1_pos2,q_c1_pos1,q_c1_pos2,ref_s_pos1_c1,1,1);
        }
        while(ref_s_pos1_c1<this->start2){
            ar.noOverlapMerge(dna,qualities,nucigar,c_c1_pos2,q_c1_pos2,ref_s_pos1_c1,1);
        }
        computeSOffset(this->cigar2_unrolled,c_c2_pos1,q_c2_pos1);
        while(ref_s_pos1_c1<=ref_e_pos2_c1 && ref_s_pos1_c1<=ref_e_pos1_c2){
            overlapMerge(ar,dna,qualities,nucigar,c_c2_pos1,c_c1_pos2,q_c2_pos1,q_c1_pos2,ref_s_pos1_c1,2,1);
        }
        if(ref_s_pos1_c1-1 == ref_e_pos2_c1 ){
            while(ref_s_pos1_c1<=ref_e_pos1_c2){
                noOverlapMerge(dna,qualities,nucigar,c_c2_pos1,q_c2_pos1,ref_s_pos1_c1,2);
            }
        } else if(ref_s_pos1_c1-1 == ref_e_pos1_c2){
            while(ref_s_pos1_c1<=ref_e_pos2_c1){
                ar.noOverlapMerge(dna,qualities,nucigar,c_c1_pos2,q_c1_pos2,ref_s_pos1_c1,1);
            }
        }
        this->start1=std::min(this->start1,ar.getStart1());
        this->end1=std::max(ar.getEnd1(),this->end2);
        this->single_end = false;
        this->cigar1=createCigar(nucigar);
        this->sequence1=ShortDnaSequence(dna,qualities);
        this->phred_sum1=phred_sum(qualities);
        this->length_incl_deletions1 = this->sequence1.size();
        this->length_incl_longdeletions1 = this->sequence1.size();
        this->cigar1_unrolled.clear();
        for (char i : nucigar){
            this->cigar1_unrolled.push_back(i);
        }
        this->start2=ar.getStart2();
        this->end2=ar.getEnd2();
        this->cigar2=ar.getCigar2();
        this->sequence2=ar.getSequence2();
        this->phred_sum2=ar.getPhredSum2();
        this->length_incl_deletions2 = ar.getSequence2().size();
        this->length_incl_longdeletions1 = ar.getSequence2().size();
        this->cigar2_unrolled =ar.getCigar2Unrolled();

   } //----------        ------------  <- this
    //                --------    ----------
    else if(ref_s_pos2_c1 <= ref_s_pos1_c2 && this->end1 < ar.getStart1() && this->start2 <= ar.getEnd1() && this->end2 >= ar.getStart2()){
        while(ref_s_pos2_c1 < ref_s_pos1_c2){
            ar.noOverlapMerge(dna,qualities,nucigar,c_c1_pos2,q_c1_pos2,ref_s_pos2_c1,1);
        }
        while(ref_s_pos2_c1 <= ar.getEnd1()){
            overlapMerge(ar,dna,qualities,nucigar,c_c2_pos1,c_c1_pos2,q_c2_pos1,q_c1_pos2,ref_s_pos2_c1,2,1);
        }
        while(ref_s_pos2_c1 < ar.getStart2()){
            noOverlapMerge(dna,qualities,nucigar,c_c2_pos1,q_c2_pos1,ref_s_pos2_c1,2);
        }
        computeSOffset(ar.getCigar2Unrolled(),c_c2_pos2,q_c2_pos2);
        while(ref_s_pos2_c1 <= ref_e_pos2_c2 && ref_s_pos2_c1 <= ref_e_pos1_c2){
            overlapMerge(ar,dna,qualities,nucigar,c_c2_pos1,c_c2_pos2,q_c2_pos1,q_c2_pos2,ref_s_pos2_c1,2,2);
        }
        if(ref_s_pos2_c1-1 == ref_e_pos2_c2){
            while(ref_s_pos2_c1<=ref_e_pos1_c2){
                noOverlapMerge(dna,qualities,nucigar,c_c2_pos1,q_c2_pos1,ref_s_pos2_c1,2);
            }
        } else if(ref_s_pos2_c1-1 == ref_e_pos1_c2){
            while(ref_s_pos2_c1<=ref_e_pos2_c2){
                ar.noOverlapMerge(dna,qualities,nucigar,c_c2_pos2,q_c2_pos2,ref_s_pos2_c1,2);
            }
        }
        this->start2=std::min(this->start2,ar.getStart1());
        this->end2=std::max(ar.getEnd2(),this->end2);
        this->single_end = false;
        this->cigar2=createCigar(nucigar);
        this->sequence2=ShortDnaSequence(dna,qualities);
        this->phred_sum2=phred_sum(qualities);
        this->length_incl_deletions2 = this->sequence2.size();
        this->length_incl_longdeletions2 = this->sequence2.size();
        this->cigar2_unrolled.clear();
        for (char i : nucigar){
            this->cigar2_unrolled.push_back(i);
        }
    } //--------      --------- <-this
      //                --  -----------
    else if(ref_s_pos1_c2<=ref_s_pos2_c1 && this->end1 < ar.getStart1() && this->start2<=ar.getEnd1() && this->end2 >= ar.getStart2() ){
        while(ref_s_pos1_c2<ref_s_pos2_c1){
            noOverlapMerge(dna,qualities,nucigar,c_c2_pos1,q_c2_pos1,ref_s_pos1_c2,2);
        }
        while(ref_s_pos1_c2<=ar.getEnd1()){
            overlapMerge(ar,dna,qualities,nucigar,c_c2_pos1,c_c1_pos2,q_c2_pos1,q_c1_pos2,ref_s_pos1_c2,2,1);
        }
        while(ref_s_pos1_c2<ar.getStart2()){
            noOverlapMerge(dna,qualities,nucigar,c_c2_pos1,q_c2_pos1,ref_s_pos1_c2,2);
        }
        computeSOffset(ar.getCigar2Unrolled(),c_c2_pos2,q_c2_pos2);
        while(ref_s_pos1_c2<=ref_e_pos1_c2 && ref_s_pos1_c2<=ref_e_pos2_c2){
            overlapMerge(ar,dna,qualities,nucigar,c_c2_pos1,c_c2_pos2,q_c2_pos1,q_c2_pos2,ref_s_pos1_c2,2,2);
        }
        if(ref_s_pos1_c2-1==ref_e_pos1_c2){
            while(ref_s_pos1_c2<=ref_e_pos2_c2){
                ar.noOverlapMerge(dna,qualities,nucigar,c_c2_pos2,q_c2_pos2,ref_s_pos1_c2,2);
            }
        } else if(ref_s_pos1_c2-1==ref_e_pos2_c2){
            while(ref_s_pos1_c2<=ref_e_pos1_c2){
                noOverlapMerge(dna,qualities,nucigar,c_c2_pos1,q_c2_pos1,ref_s_pos1_c2,2);
            }
        }
        this->start2=std::min(this->start2,ar.getStart1());
        this->end2=std::max(ar.getEnd2(),this->end2);
        this->single_end = false;
        this->cigar2=createCigar(nucigar);
        this->sequence2=ShortDnaSequence(dna,qualities);
        this->phred_sum2=phred_sum(qualities);
        this->length_incl_deletions2 = this->sequence2.size();
        this->length_incl_longdeletions2 = this->sequence2.size();
        this->cigar2_unrolled.clear();
        for (char i : nucigar){
            this->cigar2_unrolled.push_back(i);
        }

    } // -------------     ----------- <-this
        //   ----  ---------------
    else if(ref_s_pos1_c1 <= ref_s_pos2_c1 &&this->start1 <=ar.getEnd1() && this->end1 >= ar.getStart2() && this->start2 <= ar.getEnd2()){
        while(ref_s_pos1_c1<ref_s_pos2_c1){
            noOverlapMerge(dna,qualities,nucigar,c_c1_pos1,q_c1_pos1,ref_s_pos1_c1,1);
        }
        while(ref_s_pos1_c1<=ar.getEnd1()){
            overlapMerge(ar,dna,qualities,nucigar,c_c1_pos1,c_c1_pos2,q_c1_pos1,q_c1_pos2,ref_s_pos1_c1,1,1);
        }
        while(ref_s_pos1_c1<ar.getStart2()){
            noOverlapMerge(dna,qualities,nucigar,c_c1_pos1,q_c1_pos1,ref_s_pos1_c1,1);
        }
        computeSOffset(ar.getCigar2Unrolled(),c_c2_pos2,q_c2_pos2);
        while(ref_s_pos1_c1<=this->end1){
            overlapMerge(ar,dna,qualities,nucigar,c_c1_pos1,c_c2_pos2,q_c1_pos1,q_c2_pos2,ref_s_pos1_c1,1,2);
        }
        while(ref_s_pos1_c1<this->start2){
            ar.noOverlapMerge(dna,qualities,nucigar,c_c2_pos2,q_c2_pos2,ref_s_pos1_c1,2);
        }
        computeSOffset(this->cigar2_unrolled,c_c2_pos1,q_c2_pos1);
        while(ref_s_pos1_c1<=ref_e_pos2_c2 && ref_s_pos1_c1<=ref_e_pos1_c2){
            overlapMerge(ar,dna,qualities,nucigar,c_c2_pos1,c_c2_pos2,q_c2_pos1,q_c2_pos2,ref_s_pos1_c1,2,2);
        }
        if(ref_s_pos1_c1-1==ref_e_pos2_c2){
            while(ref_s_pos1_c1<=ref_e_pos1_c2){
                noOverlapMerge(dna,qualities,nucigar,c_c2_pos1,q_c2_pos1,ref_s_pos1_c1,2);
            }
        } else if(ref_s_pos1_c1-1==ref_e_pos1_c2){
            while(ref_s_pos1_c1<=ref_e_pos2_c2){
                ar.noOverlapMerge(dna,qualities,nucigar,c_c2_pos2,q_c2_pos2,ref_s_pos1_c1,2);
            }
        }
        this->start1=std::min(this->start1,ar.getStart1());
        this->end1=std::max(ar.getEnd2(),this->end2);
        this->single_end = true;
        this->cigar1=createCigar(nucigar);
        this->sequence1=ShortDnaSequence(dna,qualities);
        this->phred_sum1=phred_sum(qualities);
        this->length_incl_deletions1 = this->sequence1.size();
        this->length_incl_longdeletions1 = this->sequence1.size();
        this->cigar1_unrolled.clear();
        for (char i : nucigar){
            this->cigar1_unrolled.push_back(i);
        }
    } //----------         -----------  <-this
     //  ----   -------
    else if(ref_s_pos1_c1 <= ref_s_pos2_c1 && this->start2 > ar.getEnd2() && this->end1 >= ar.getStart2() && this->start1 <= ar.getEnd1()){
        while(ref_s_pos1_c1 < ref_s_pos2_c1){
            noOverlapMerge(dna,qualities,nucigar,c_c1_pos1,q_c1_pos1,ref_s_pos1_c1,1);
        }
        while(ref_s_pos1_c1 <= ar.getEnd1()){
            overlapMerge(ar,dna,qualities,nucigar,c_c1_pos1,c_c1_pos2,q_c1_pos1,q_c1_pos2,ref_s_pos1_c1,1,1);
        }
        while(ref_s_pos1_c1<ar.getStart2()){
            noOverlapMerge(dna,qualities,nucigar,c_c1_pos1,q_c1_pos1,ref_s_pos1_c1,1);
        }
        computeSOffset(ar.getCigar2Unrolled(),c_c2_pos2,q_c2_pos2);
        while(ref_s_pos1_c1<=ref_e_pos1_c1 && ref_s_pos1_c1<=ref_e_pos2_c2){
            overlapMerge(ar,dna,qualities,nucigar,c_c1_pos1,c_c2_pos2,q_c2_pos1,q_c2_pos2,ref_s_pos1_c1,1,2);
        }
        if(ref_s_pos1_c1-1==ref_e_pos1_c1){
            while(ref_s_pos1_c1<=ref_e_pos2_c2){
                ar.noOverlapMerge(dna,qualities,nucigar,c_c2_pos2,q_c2_pos2,ref_s_pos1_c1,2);
            }
        } else if(ref_s_pos1_c1-1==ref_e_pos2_c2){
            while(ref_s_pos1_c1<=ref_e_pos1_c1){
                noOverlapMerge(dna,qualities,nucigar,c_c1_pos1,q_c1_pos1,ref_s_pos1_c1,1);
            }
        }
        this->start1=std::min(this->start1,ar.getStart1());
        this->end1=std::max(ar.getEnd2(),this->end1);
        this->single_end = false;
        this->cigar1=createCigar(nucigar);
        this->sequence1=ShortDnaSequence(dna,qualities);
        this->phred_sum1=phred_sum(qualities);
        this->length_incl_deletions1 = this->sequence1.size();
        this->length_incl_longdeletions1 = this->sequence1.size();
        this->cigar1_unrolled.clear();
        for (char i : nucigar){
            this->cigar1_unrolled.push_back(i);
        }
    } //   ------   -----------   <-this
    //----------------  ----------------
      else if(ref_s_pos2_c1 <= ref_s_pos1_c1 && this->end1 >= ar.getStart1() && this->start2 <= ar.getEnd1() && this->end2 >= ar.getStart2()){
        while(ref_s_pos2_c1 < ref_s_pos1_c1){
            ar.noOverlapMerge(dna,qualities,nucigar,c_c1_pos2,q_c1_pos2,ref_s_pos2_c1,1);
        }
        while(ref_s_pos2_c1 <=this->end1){
            overlapMerge(ar,dna,qualities,nucigar,c_c1_pos1,c_c1_pos2,q_c1_pos1,q_c1_pos2,ref_s_pos2_c1,1,1);
        }
        while(ref_s_pos2_c1 < this->start2){
            ar.noOverlapMerge(dna,qualities,nucigar,c_c1_pos2,q_c1_pos2,ref_s_pos2_c1,1);
        }
        computeSOffset(this->cigar2_unrolled,c_c2_pos1,q_c2_pos1);
        while(ref_s_pos2_c1<=ar.getEnd1()){
            overlapMerge(ar,dna,qualities,nucigar,c_c2_pos1,c_c1_pos2,q_c2_pos1,q_c1_pos2,ref_s_pos2_c1,2,1);
        }
        while(ref_s_pos2_c1<ar.getStart2()){
            noOverlapMerge(dna,qualities,nucigar,c_c2_pos1,q_c2_pos1,ref_s_pos2_c1,2);
        }
        computeSOffset(ar.getCigar2Unrolled(),c_c2_pos2,q_c2_pos2);
        while(ref_s_pos2_c1<=ref_e_pos1_c2 && ref_s_pos2_c1<=ref_e_pos2_c2){
            overlapMerge(ar,dna,qualities,nucigar,c_c2_pos1,c_c2_pos2,q_c2_pos1,q_c2_pos2,ref_s_pos2_c1,2,2);
        }
        if(ref_s_pos2_c1-1 == ref_e_pos1_c2){
            while(ref_s_pos2_c1<=ref_e_pos2_c2){
                ar.noOverlapMerge(dna,qualities,nucigar,c_c2_pos2,q_c2_pos2,ref_s_pos2_c1,2);
            }
        } else if(ref_s_pos2_c1-1 == ref_e_pos2_c2){
            while(ref_s_pos2_c1<=ref_e_pos1_c2){
                noOverlapMerge(dna,qualities,nucigar,c_c2_pos1,q_c2_pos1,ref_s_pos2_c1,2);
            }
        }
        this->start1=std::min(this->start1,ar.getStart1());
        this->end1=std::max(ar.getEnd2(),this->end2);
        this->single_end = true;
        this->cigar1=createCigar(nucigar);
        this->sequence1=ShortDnaSequence(dna,qualities);
        this->phred_sum1=phred_sum(qualities);
        this->length_incl_deletions1 = this->sequence1.size();
        this->length_incl_longdeletions1 = this->sequence1.size();
        this->cigar1_unrolled.clear();
        for (char i : nucigar){
            this->cigar1_unrolled.push_back(i);
        }
    } //      ----    --------- <-this
      //------------------          --------------
      else if(ref_s_pos2_c1 <= ref_s_pos1_c1 && this->end1>=ar.getStart1() && this->start2 <=ar.getEnd1() && this->end2 < ar.getStart2()){
        while(ref_s_pos2_c1<ref_s_pos1_c1){
            ar.noOverlapMerge(dna,qualities,nucigar,c_c1_pos2,q_c1_pos2,ref_s_pos2_c1,1);
        }
        while(ref_s_pos2_c1<=this->end1){
            overlapMerge(ar,dna,qualities,nucigar,c_c1_pos1,c_c1_pos2,q_c1_pos1,q_c1_pos2,ref_s_pos2_c1,1,1);
        }
        while(ref_s_pos2_c1<this->start2){
            ar.noOverlapMerge(dna,qualities,nucigar,c_c1_pos2,q_c1_pos2,ref_s_pos2_c1,1);
        }
        computeSOffset(this->cigar2_unrolled,c_c2_pos1,q_c2_pos1);
        while(ref_s_pos2_c1<=ref_e_pos2_c1 && ref_s_pos2_c1<=ref_e_pos1_c2){
            overlapMerge(ar,dna,qualities,nucigar,c_c2_pos1,c_c1_pos2,q_c2_pos1,q_c1_pos2,ref_s_pos2_c1,2,1);
        }
        if(ref_s_pos2_c1-1==ref_e_pos2_c1){
            while(ref_s_pos2_c1<=ref_e_pos1_c2){
                noOverlapMerge(dna,qualities,nucigar,c_c2_pos1,q_c2_pos1,ref_s_pos2_c1,2);
            }
        } else if(ref_s_pos2_c1-1 == ref_e_pos1_c2){
            while(ref_s_pos2_c1<=ref_e_pos2_c1){
                ar.noOverlapMerge(dna,qualities,nucigar,c_c1_pos2,q_c1_pos2,ref_s_pos2_c1,1);
            }
        }
        this->start1=std::min(this->start1,ar.getStart1());
        this->end1=std::max(ar.getEnd1(),this->end2);
        this->single_end = false;
        this->cigar1=createCigar(nucigar);
        this->sequence1=ShortDnaSequence(dna,qualities);
        this->phred_sum1=phred_sum(qualities);
        this->length_incl_deletions1 = this->sequence1.size();
        this->length_incl_longdeletions1 = this->sequence1.size();
        this->cigar1_unrolled.clear();
        for (char i : nucigar){
            this->cigar1_unrolled.push_back(i);
        }
        this->start2=ar.getStart2();
        this->end2=ar.getEnd2();
        this->cigar2=ar.getCigar2();
        this->sequence2=ar.getSequence2();
        this->phred_sum2=ar.getPhredSum2();
        this->length_incl_deletions2 = ar.getSequence2().size();
        this->length_incl_longdeletions1 = ar.getSequence2().size();
        this->cigar2_unrolled =ar.getCigar2Unrolled();

    } //                -------    ------- <-this
      //-------------       ------------
      else if(ref_s_pos1_c1 <= ref_s_pos2_c2 && this->start1 > ar.getEnd1() && this->end1>=ar.getStart2() && this->start2 <= ar.getEnd2()){
        while(ref_s_pos1_c1<ref_s_pos2_c2){
            noOverlapMerge(dna,qualities,nucigar,c_c1_pos1,q_c1_pos1,ref_s_pos1_c1,1);
        }
        while(ref_s_pos1_c1<=this->end1){
            overlapMerge(ar,dna,qualities,nucigar,c_c1_pos1,c_c2_pos2,q_c1_pos1,q_c2_pos2,ref_s_pos1_c1,1,2);
        }
        while(ref_s_pos1_c1<this->start2){
            ar.noOverlapMerge(dna,qualities,nucigar,c_c2_pos2,q_c2_pos2,ref_s_pos1_c1,2);
        }
        computeSOffset(this->cigar2_unrolled,c_c2_pos1,q_c2_pos1);
        while(ref_s_pos1_c1<= ref_e_pos1_c2 && ref_s_pos1_c1<= ref_e_pos2_c2){
            overlapMerge(ar,dna,qualities,nucigar,c_c2_pos1,c_c2_pos2,q_c2_pos1,q_c2_pos2,ref_s_pos1_c1,2,2);
        }
        if(ref_s_pos1_c1-1 == ref_e_pos1_c2){
            while(ref_s_pos1_c1<=ref_e_pos2_c2){
                ar.noOverlapMerge(dna,qualities,nucigar,c_c2_pos2,q_c2_pos2,ref_s_pos1_c1,2);
            }
        } else if(ref_s_pos1_c1-1 == ref_e_pos2_c2){
            while(ref_s_pos1_c1<=ref_e_pos1_c2){
                noOverlapMerge(dna,qualities,nucigar,c_c2_pos1,q_c2_pos1,ref_s_pos1_c1,2);
            }
        }
        this->start1=ar.getStart1();
        this->end1=ar.getEnd1();
        this->cigar1=ar.getCigar1();
        this->sequence1=ar.getSequence1();
        this->phred_sum1=ar.getPhredSum1();
        this->length_incl_deletions1 = ar.getSequence1().size();
        this->length_incl_longdeletions1 = ar.getSequence1().size();
        this->cigar1_unrolled =ar.getCigar1Unrolled();

        this->start2=std::min(this->start1,ar.getStart2());
        this->end2=std::max(ar.getEnd2(),this->end2);
        this->single_end = false;
        this->cigar2=createCigar(nucigar);
        this->sequence2=ShortDnaSequence(dna,qualities);
        this->phred_sum2=phred_sum(qualities);
        this->length_incl_deletions2 = this->sequence2.size();
        this->length_incl_longdeletions2 = this->sequence2.size();
        this->cigar2_unrolled.clear();
        for (char i : nucigar){
            this->cigar2_unrolled.push_back(i);
        }
    } //                   ---   -------- <-this
      //-------------     -----------
     else if(ref_s_pos2_c2<=ref_s_pos1_c1 && ar.getEnd1()<this->start1 && this->end1>=ar.getStart2() && this->start2 <= ar.getEnd2()){
        while(ref_s_pos2_c2<ref_s_pos1_c1){
            ar.noOverlapMerge(dna,qualities,nucigar,c_c2_pos2,q_c2_pos2,ref_s_pos2_c2,2);
        }
        while(ref_s_pos2_c2<=this->end1){
            overlapMerge(ar,dna,qualities,nucigar,c_c1_pos1,c_c2_pos2,q_c1_pos1,q_c2_pos2,ref_s_pos2_c2,1,2);
        }
        while(ref_s_pos2_c2<this->start2){
            ar.noOverlapMerge(dna,qualities,nucigar,c_c2_pos2,q_c2_pos2,ref_s_pos2_c2,2);
        }
        computeSOffset(this->cigar2_unrolled,c_c2_pos1,q_c2_pos1);
        while(ref_s_pos2_c2<=ref_e_pos1_c2 && ref_s_pos2_c2<=ref_e_pos2_c2){
            overlapMerge(ar,dna,qualities,nucigar,c_c2_pos1,c_c2_pos2,q_c2_pos1,q_c2_pos2,ref_s_pos2_c2,2,2);
        }
        if(ref_s_pos2_c2-1==ref_e_pos1_c2){
            while(ref_s_pos2_c2<=ref_e_pos2_c2){
                ar.noOverlapMerge(dna,qualities,nucigar,c_c2_pos2,q_c2_pos2,ref_s_pos2_c2,2);
            }
        } else if(ref_s_pos2_c2-1==ref_e_pos2_c2){
            while(ref_s_pos2_c2<=ref_e_pos1_c2){
                noOverlapMerge(dna,qualities,nucigar,c_c2_pos1,q_c2_pos1,ref_s_pos2_c2,2);
            }
        }
        this->start1=ar.getStart1();
        this->end1=ar.getEnd1();
        this->cigar1=ar.getCigar1();
        this->sequence1=ar.getSequence1();
        this->phred_sum1=ar.getPhredSum1();
        this->length_incl_deletions1 = this->sequence1.size();
        this->length_incl_longdeletions1 = this->sequence1.size();
        this->cigar1_unrolled = ar.getCigar1Unrolled();

        this->start2=std::min(this->start1,ar.getStart2());
        this->end2=std::max(ar.getEnd2(),this->end2);
        this->single_end = false;
        this->cigar2=createCigar(nucigar);
        this->sequence2=ShortDnaSequence(dna,qualities);
        this->phred_sum2=phred_sum(qualities);
        this->length_incl_deletions2 = this->sequence2.size();
        this->length_incl_longdeletions2 = this->sequence2.size();
        this->cigar2_unrolled.clear();
        for (char i : nucigar){
            this->cigar2_unrolled.push_back(i);
        }
    }
       //  --------    ------------  <-this
       //-----    ----------------
      else if(ref_s_pos2_c1 <= ref_s_pos1_c1 && this->start1 <= ar.getEnd1() && this->end1 >= ar.getStart2() && this->start2 <= ar.getEnd2()){
        while(ref_s_pos2_c1<ref_s_pos1_c1){
            ar.noOverlapMerge(dna,qualities,nucigar,c_c1_pos2,q_c1_pos2,ref_s_pos2_c1,1);
        }
        while(ref_s_pos2_c1<=ar.getEnd1()){
            overlapMerge(ar,dna,qualities,nucigar,c_c1_pos1,c_c1_pos2,q_c1_pos1,q_c1_pos2,ref_s_pos2_c1,1,1);
        }
        while(ref_s_pos2_c1<ar.getStart2()){
            noOverlapMerge(dna,qualities,nucigar,c_c1_pos1,q_c1_pos1,ref_s_pos2_c1,1);
        }
        computeSOffset(ar.getCigar2Unrolled(),c_c2_pos2,q_c2_pos2);
        while(ref_s_pos2_c1<=this->end1){
            overlapMerge(ar,dna,qualities,nucigar,c_c1_pos1,c_c2_pos2,q_c1_pos1,q_c2_pos2,ref_s_pos2_c1,1,2);
        }
        while(ref_s_pos2_c1<this->start2){
            ar.noOverlapMerge(dna,qualities,nucigar,c_c2_pos2,q_c2_pos2,ref_s_pos2_c1,2);
        }
        computeSOffset(this->cigar2_unrolled,c_c2_pos1,q_c2_pos1);
        while(ref_s_pos2_c1<=ref_e_pos2_c2 && ref_s_pos2_c1<=ref_e_pos1_c2){
            overlapMerge(ar,dna,qualities,nucigar,c_c2_pos1,c_c2_pos2,q_c2_pos1,q_c2_pos2,ref_s_pos2_c1,2,2);
        }
        if(ref_s_pos2_c1-1==ref_e_pos2_c2){
            while(ref_s_pos2_c1<=ref_e_pos1_c2){
                   noOverlapMerge(dna,qualities,nucigar,c_c2_pos1,q_c2_pos1,ref_s_pos2_c1,2);
            }
        } else if(ref_s_pos2_c1-1==ref_e_pos1_c2){
            while(ref_s_pos2_c1<=ref_e_pos2_c2){
                ar.noOverlapMerge(dna,qualities,nucigar,c_c2_pos2,q_c2_pos2,ref_s_pos2_c1,2);
            }
        }
        this->start1=std::min(this->start1,ar.getStart1());
        this->end1=std::max(ar.getEnd2(),this->end2);
        this->single_end = true;
        this->cigar1=createCigar(nucigar);
        this->sequence1=ShortDnaSequence(dna,qualities);
        this->phred_sum1=phred_sum(qualities);
        this->length_incl_deletions1 = this->sequence1.size();
        this->length_incl_longdeletions1 = this->sequence1.size();
        this->cigar1_unrolled.clear();
        for (char i : nucigar){
            this->cigar1_unrolled.push_back(i);
        }
    } //   --------------                 -------------- <-this
      //----------     -------------
      else if(ref_s_pos2_c1<=ref_s_pos1_c1 && this->start2 > ar.getEnd2() && this->start1 <= ar.getEnd1() && this->end1 >= ar.getStart2()){
        while(ref_s_pos2_c1<ref_s_pos1_c1){
            ar.noOverlapMerge(dna,qualities,nucigar,c_c1_pos2,q_c1_pos2,ref_s_pos2_c1,1);
        }
        while(ref_s_pos2_c1<=ar.getEnd1()){
            overlapMerge(ar,dna,qualities,nucigar,c_c1_pos1,c_c1_pos2,q_c1_pos1,q_c1_pos2,ref_s_pos2_c1,1,1);
        }
        while(ref_s_pos2_c1<ar.getStart2()){
            noOverlapMerge(dna,qualities,nucigar,c_c1_pos1,q_c1_pos1,ref_s_pos2_c1,1);
        }
        computeSOffset(ar.getCigar2Unrolled(),c_c2_pos2,q_c2_pos2);
        while(ref_s_pos2_c1<=ref_e_pos1_c1 && ref_s_pos2_c1<=ref_e_pos2_c2){
            overlapMerge(ar,dna,qualities,nucigar,c_c1_pos1,c_c2_pos2,q_c1_pos1,q_c2_pos2,ref_s_pos2_c1,1,2);
        }
        if(ref_s_pos2_c1-1==ref_e_pos1_c1){
            while(ref_s_pos2_c1<=ref_e_pos2_c2){
                ar.noOverlapMerge(dna,qualities,nucigar,c_c2_pos2,q_c2_pos2,ref_s_pos2_c1,2);
            }
        } else if(ref_s_pos2_c1-1==ref_e_pos2_c2){
            while(ref_s_pos2_c1<=ref_e_pos1_c1){
                noOverlapMerge(dna,qualities,nucigar,c_c1_pos1,q_c1_pos1,ref_s_pos2_c1,1);
            }
        }
        this->start1=std::min(this->start1,ar.getStart1());
        this->end1=std::max(ar.getEnd2(),this->end1);
        this->single_end = false;
        this->cigar1=createCigar(nucigar);
        this->sequence1=ShortDnaSequence(dna,qualities);
        this->phred_sum1=phred_sum(qualities);
        this->length_incl_deletions1 = this->sequence1.size();
        this->length_incl_longdeletions1 = this->sequence1.size();
        this->cigar1_unrolled.clear();
        for (char i : nucigar){
            this->cigar1_unrolled.push_back(i);
        }
    }
}

void AlignmentRecord::mergeAlignmentRecordsMixed(const AlignmentRecord& ar){
    if(ar.isSingleEnd()){
        std::string dna, qualities, nucigar = "";
        int offset_s_f, offset_s_b, offset_p_f1, offset_p_f2, offset_p_b1, offset_p_b2 = 0;
        //get starting position and ending position according to ref position, paying attention to clipped bases
        //updated ref position including clips
        offset_s_f = computeOffset(ar.getCigar1Unrolled());
        offset_s_b = computeRevOffset(ar.getCigar1Unrolled());
        offset_p_f1 = computeOffset(this->cigar1_unrolled);
        offset_p_f2 = computeOffset(this->cigar2_unrolled);
        offset_p_b1 = computeRevOffset(this->cigar1_unrolled);
        offset_p_b2 = computeRevOffset(this->cigar2_unrolled);
        int ref_s_pos1 = ar.getStart1()-offset_s_f;
        int ref_e_pos1 = ar.getEnd1()+offset_s_b;
        int ref_p_s_pos1 = this->start1-offset_p_f1;
        int ref_p_e_pos1 = this->end1+offset_p_b1;
        int ref_p_s_pos2 = this->start2-offset_p_f2;
        int ref_p_e_pos2 = this->end2+offset_p_b2;
        //position in query sequences // phred scores
        int q_pos1 = 0;
        int q_p_pos1 = 0;
        int q_p_pos2 = 0;
        //position in unrolled cigar vectors
        int c_pos1 = 0;
        int c_p_pos1 = 0;
        int c_p_pos2 = 0;
        //int i;
        // ---------     -------- ->this (second read not changed)
        //----------
        if(ar.getEnd1() < this->start2){
            mergeAlignmentRecordsSingle(ar,1,1);
            this->single_end = false;
            assert(this->end1 < this->start2);
        } // -------     -------- ->this (second read not changed)
        //             ----------
        else if (ar.getStart1() > this->end1){
            mergeAlignmentRecordsSingle(ar,2,1);
            assert(this->end1 < this->start2);
        } //----------          -----------   ->this OR  -------       -----------
        //-------------------------------              -----------------------------
        else if(ref_s_pos1 <= ref_p_s_pos1){
            while(ref_s_pos1<ref_p_s_pos1){
                ar.noOverlapMerge(dna, qualities, nucigar, c_pos1, q_pos1,ref_s_pos1,1);
            }
            while(ref_s_pos1<=this->end1){
                overlapMerge(ar,dna,qualities,nucigar,c_p_pos1,c_pos1,q_p_pos1,q_pos1,ref_s_pos1,1,1);
            }
            while(ref_s_pos1<this->start2){
                ar.noOverlapMerge(dna,qualities,nucigar,c_pos1,q_pos1,ref_s_pos1,1);
            }
            computeSOffset(this->cigar2_unrolled,c_p_pos2,q_p_pos2);
            while(ref_s_pos1<=ref_p_e_pos2 && ref_s_pos1 <= ref_e_pos1){
                overlapMerge(ar,dna,qualities,nucigar,c_p_pos2,c_pos1,q_p_pos2,q_pos1,ref_s_pos1,2,1);
            }
            if(ref_s_pos1-1 == ref_p_e_pos2){
                while(ref_s_pos1<=ref_e_pos1){
                    ar.noOverlapMerge(dna,qualities,nucigar,c_pos1,q_pos1,ref_s_pos1,1);
                }
            } else if (ref_s_pos1-1 == ref_e_pos1){
                while(ref_s_pos1<=ref_p_e_pos2){
                    noOverlapMerge(dna,qualities,nucigar,c_p_pos2,q_p_pos2,ref_s_pos1,2);
                }
            }
            this->start1=std::min(this->start1,ar.getStart1());
            this->end1=std::max(ar.getEnd1(),this->end2);
            this->single_end = true;
            this->cigar1=createCigar(nucigar);
            this->sequence1=ShortDnaSequence(dna,qualities);
            this->phred_sum1=phred_sum(qualities);
            this->length_incl_deletions1 = this->sequence1.size();
            this->length_incl_longdeletions1 = this->sequence1.size();
            this->cigar1_unrolled.clear();
            for (char i : nucigar){
                this->cigar1_unrolled.push_back(i);
            }
        } //----------          ------------ ->this OR ----------       -----------
        //     -------------------------------            ----------------------
        else if(ref_s_pos1 >= ref_p_s_pos1){
            while(ref_p_s_pos1 < ref_s_pos1){
                noOverlapMerge(dna,qualities,nucigar, c_p_pos1,q_p_pos1,ref_p_s_pos1,1);
            }
            while(ref_p_s_pos1<=this->end1){
                overlapMerge(ar,dna,qualities,nucigar,c_p_pos1,c_pos1,q_p_pos1,q_pos1,ref_p_s_pos1,1,1);
            }
            while(ref_p_s_pos1<this->start2){
                ar.noOverlapMerge(dna,qualities,nucigar,c_pos1,q_pos1,ref_p_s_pos1,1);
            }
            computeSOffset(this->cigar2_unrolled,c_p_pos2,q_p_pos2);
            while(ref_p_s_pos1<=ref_p_e_pos2 && ref_p_s_pos1 <= ref_e_pos1){
                overlapMerge(ar,dna,qualities,nucigar,c_p_pos2,c_pos1,q_p_pos2,q_pos1,ref_p_s_pos1,2,1);
            }
            if(ref_p_s_pos1-1 == ref_p_e_pos2){
                while(ref_p_s_pos1<=ref_e_pos1){
                    ar.noOverlapMerge(dna,qualities,nucigar,c_pos1,q_pos1,ref_p_s_pos1,1);
                }
            } else if (ref_p_s_pos1-1 == ref_e_pos1){
                while(ref_p_s_pos1<=ref_p_e_pos2){
                    noOverlapMerge(dna,qualities,nucigar,c_p_pos2,q_p_pos2,ref_p_s_pos1,2);
                }
            }
            this->start1=std::min(this->start1,ar.getStart1());
            this->end1=std::max(ar.getEnd1(),this->end2);
            this->single_end = true;
            this->cigar1=createCigar(nucigar);
            this->sequence1=ShortDnaSequence(dna,qualities);
            this->phred_sum1=phred_sum(qualities);
            this->length_incl_deletions1 = this->sequence1.size();
            this->length_incl_longdeletions1 = this->sequence1.size();
            this->cigar1_unrolled.clear();
            for (char i : nucigar){
                this->cigar1_unrolled.push_back(i);
            }
        }
    }
    else if (ar.isPairedEnd()){
        std::string dna, qualities, nucigar = "";
        int offset_s_f, offset_s_b, offset_p_f1, offset_p_f2, offset_p_b1, offset_p_b2 = 0;
        //get starting position and ending position according to ref position, paying attention to clipped bases
        //updated ref position including clips
        offset_s_f = computeOffset(this->cigar1_unrolled);
        offset_s_b = computeRevOffset(this->cigar1_unrolled);
        offset_p_f1 = computeOffset(ar.getCigar1Unrolled());
        offset_p_f2 = computeOffset(ar.getCigar2Unrolled());
        offset_p_b1 = computeRevOffset(ar.getCigar1Unrolled());
        offset_p_b2 = computeRevOffset(ar.getCigar2Unrolled());
        int ref_s_pos1 = this->start1-offset_s_f;
        int ref_e_pos1 = this->end1+offset_s_b;
        int ref_p_s_pos1 = ar.getStart1()-offset_p_f1;
        //int ref_p_e_pos1 = ar.getEnd1()+offset_p_b1;
        //int ref_p_s_pos2 = ar.getStart2()-offset_p_f2;
        int ref_p_e_pos2 = ar.getEnd2()+offset_p_b2;
        //position in query sequences // phred scores
        int q_pos1 = 0;
        int q_p_pos1 = 0;
        int q_p_pos2 = 0;
        //position in unrolled cigar vectors
        int c_pos1 = 0;
        int c_p_pos1 = 0;
        int c_p_pos2 = 0;
        // ---------  -----------          OR ---------- ------------
        // ---------               ->this                -------------
        if(this->end1 < ar.getStart2()){
            mergeAlignmentRecordsSingle(ar,1,1);
            this->start2 = ar.getStart2();
            this->end2=ar.getEnd2();
            this->single_end= false;
            this->cigar2 = ar.getCigar2();
            this->sequence2=ar.getSequence2();
            this->phred_sum2=ar.getPhredSum2();
            this->length_incl_deletions2 = ar.getLengthInclDeletions2();
            this->length_incl_longdeletions2 = ar.getLengthInclLongDeletions2();
            this->cigar2_unrolled = ar.getCigar2Unrolled();
        } else if (this->start1 > ar.getEnd1()){
            mergeAlignmentRecordsSingle(ar,1,2);
            this->start1= ar.getStart1();
            this->end1=ar.getEnd1();
            this->single_end= false;
            this->cigar1 = ar.getCigar1();
            this->sequence1=ar.getSequence1();
            this->phred_sum1=ar.getPhredSum1();
            this->length_incl_deletions1 = ar.getLengthInclDeletions1();
            this->length_incl_longdeletions1 = ar.getLengthInclLongDeletions1();
            this->cigar1_unrolled = ar.getCigar1Unrolled();
        }//----------          -----------        OR  -------       -----------
        //----------------------------    <-this    -----------------------------
        else if(ref_s_pos1 <= ref_p_s_pos1){
                while(ref_s_pos1<ref_p_s_pos1){
                    noOverlapMerge(dna, qualities, nucigar, c_pos1, q_pos1,ref_s_pos1,1);
                }
                while(ref_s_pos1<=ar.getEnd1()){
                    overlapMerge(ar,dna,qualities,nucigar,c_pos1,c_p_pos1,q_pos1,q_p_pos1,ref_s_pos1,1,1);
                }
                while(ref_s_pos1<ar.getStart2()){
                    noOverlapMerge(dna,qualities,nucigar,c_pos1,q_pos1,ref_s_pos1,1);
                }
                computeSOffset(ar.getCigar2Unrolled(),c_p_pos2,q_p_pos2);
                while(ref_s_pos1<=ref_p_e_pos2 && ref_s_pos1 <= ref_e_pos1){
                    overlapMerge(ar,dna,qualities,nucigar,c_pos1,c_p_pos2,q_pos1,q_p_pos2,ref_s_pos1,1,2);
                }
                if(ref_s_pos1-1 == ref_p_e_pos2){
                    while(ref_s_pos1<=ref_e_pos1){
                        noOverlapMerge(dna,qualities,nucigar,c_pos1,q_pos1,ref_s_pos1,1);
                    }
                } else if (ref_s_pos1-1 == ref_e_pos1){
                    while(ref_s_pos1<=ref_p_e_pos2){
                        ar.noOverlapMerge(dna,qualities,nucigar,c_p_pos2,q_p_pos2,ref_s_pos1,2);
                    }
                }
                this->start1=std::min(this->start1,ar.getStart1());
                this->end1=std::max(ar.getEnd2(),this->end1);
                this->single_end = true;
                this->cigar1=createCigar(nucigar);
                this->sequence1=ShortDnaSequence(dna,qualities);
                this->phred_sum1=phred_sum(qualities);
                this->length_incl_deletions1 = this->sequence1.size();
                this->length_incl_longdeletions1 = this->sequence1.size();
                this->cigar1_unrolled.clear();
                for (char i : nucigar){
                    this->cigar1_unrolled.push_back(i);
                }

        }//----------          ------------        OR ----------       -----------
        //     -------------------------------   <-this   ----------------------
        else if(ref_s_pos1 >= ref_p_s_pos1){
            while(ref_p_s_pos1 < ref_s_pos1){
                ar.noOverlapMerge(dna,qualities,nucigar, c_p_pos1,q_p_pos1,ref_p_s_pos1,1);
            }
            while(ref_p_s_pos1<=ar.getEnd1()){
                overlapMerge(ar,dna,qualities,nucigar,c_pos1,c_p_pos1,q_pos1,q_p_pos1,ref_p_s_pos1,1,1);
            }
            while(ref_p_s_pos1<ar.getStart2()){
                noOverlapMerge(dna,qualities,nucigar,c_pos1,q_pos1,ref_p_s_pos1,1);
            }
            computeSOffset(ar.getCigar2Unrolled(),c_p_pos2,q_p_pos2);
            while(ref_p_s_pos1<=ref_p_e_pos2 && ref_p_s_pos1 <= ref_e_pos1){
                overlapMerge(ar,dna,qualities,nucigar,c_pos1,c_p_pos2,q_pos1,q_p_pos2,ref_p_s_pos1,1,2);
            }
            if(ref_p_s_pos1-1 == ref_p_e_pos2){
                while(ref_p_s_pos1<=ref_e_pos1){
                    noOverlapMerge(dna,qualities,nucigar,c_pos1,q_pos1,ref_p_s_pos1,1);
                }
            } else if (ref_p_s_pos1-1 == ref_e_pos1){
                while(ref_p_s_pos1<=ref_p_e_pos2){
                    ar.noOverlapMerge(dna,qualities,nucigar,c_p_pos2,q_p_pos2,ref_p_s_pos1,2);
                }
            }
            this->start1=std::min(this->start1,ar.getStart1());
            this->end1=std::max(ar.getEnd2(),this->end1);
            this->single_end = true;
            this->cigar1=createCigar(nucigar);
            this->sequence1=ShortDnaSequence(dna,qualities);
            this->phred_sum1=phred_sum(qualities);
            this->length_incl_deletions1 = this->sequence1.size();
            this->length_incl_longdeletions1 = this->sequence1.size();
            this->cigar1_unrolled.clear();
            for (char i : nucigar){
                this->cigar1_unrolled.push_back(i);
            }
        }
    }
}

//helper functions for merging DNA Sequences to create combined Alignment Record
void AlignmentRecord::noOverlapMerge(std::string& dna, std::string& qualities, std::string& nucigar, int& c_pos, int& q_pos, int& ref_pos, int i) const{
    char c;
    const ShortDnaSequence* s = 0;
    if (i == 1){
        c = this->cigar1_unrolled[c_pos];
        s = &this->sequence1;
    } else {
        c = this->cigar2_unrolled[c_pos];
        s = &this->sequence2;
    }
    if (c == 'H'){
        ref_pos++;
        c_pos++;
    } else if (c == 'I') {
        dna += (*s)[q_pos];
        qualities += s->qualityChar(q_pos);
        nucigar += 'I';
        q_pos++;
        c_pos++;
    } else if (c == 'D') {
        nucigar += 'D';
        ref_pos++;
        c_pos++;
    } else if (c == 'S'){
        ref_pos++;
        q_pos++;
        c_pos++;
    } else if (c == 'M'){
        dna += (*s)[q_pos];
        qualities += s->qualityChar(q_pos);
        nucigar += c;
        ref_pos++;
        q_pos++;
        c_pos++;
    } else {
        assert(false);
        /*cout << "Cigar string contains inappropriate character: " << c << endl;
        cout <<  dna << endl;
        cout << qualities << endl;
        cout << nucigar << endl;
        dna += (*s)[q_pos];
        qualities += s->qualityChar(q_pos);
        nucigar += c;
        ref_pos++;
        q_pos++;
        c_pos++;*/
    }
}



//helper function for mergeAlignmentRecords function
void AlignmentRecord::overlapMerge(const AlignmentRecord& ar, std::string& dna, std::string& qualities, std::string& nucigar, int& c_pos1, int& c_pos2, int& q_pos1, int& q_pos2, int& ref_pos, int i, int j) const{
    char c1, c2;
    const ShortDnaSequence* s1,* s2 = 0;
    if (i == 1){
        c1 = this->cigar1_unrolled[c_pos1];
        s1 = &this->sequence1;
        if(j == 1){
            c2 = ar.getCigar1Unrolled()[c_pos2];
            s2 = &ar.getSequence1();
        } else {
            c2 = ar.getCigar2Unrolled()[c_pos2];
            s2 = &ar.getSequence2();
        }
    } else {
        c1 = this->cigar2_unrolled[c_pos1];
        s1 = &this->sequence2;
        if(j == 1){
            c2 = ar.getCigar1Unrolled()[c_pos2];
            s2 = &ar.getSequence1();
        } else {
            c2 = ar.getCigar2Unrolled()[c_pos2];
            s2 = &ar.getSequence2();
        }
    }

    if((c1 == 'M' && c2 == 'M') || (c1 == 'S' && c2 == 'S') || (c1 == 'I' && c2 == 'I')){
        if (c1 != 'S'){
            std::pair<char,char> resPair = computeEntry((*s1)[q_pos1],s1->qualityChar(q_pos1),(*s2)[q_pos2],s2->qualityChar(q_pos2));
            dna += resPair.first;
            qualities += resPair.second;
            nucigar += c1;
        }
        if (c1 != 'I') ref_pos++;
        q_pos1++;
        q_pos2++;
        c_pos1++;
        c_pos2++;
    } else if ((c1 == 'D' && c2 == 'D') || (c1 == 'H' && c2 == 'H') || (c1 == 'D' && c2 == 'H') || (c1 == 'H' && c2 == 'D') || (c1 == 'D' && c2 == 'S') || (c1 == 'S' && c2 == 'D')){
        c_pos1++;
        c_pos2++;
        ref_pos++;
        if (c1 == 'D' || c2 == 'D'){
            nucigar += 'D';
        }
        if (c1 == 'S'){
            q_pos1++;
        } else if(c2 == 'S'){
            q_pos2++;
        }
    } else if ((c1 == 'M' && (c2 == 'D' || c2 == 'H' || c2 == 'S')) || ((c1 == 'D' || c1 == 'H' || c1 == 'S') && c2 == 'M') || (c1 == 'S' && c2 == 'H') || (c1 == 'H' && c2 == 'S')) {
        if (c1 == 'M'){
            nucigar += 'M';
            dna += (*s1)[q_pos1];
            qualities += s1->qualityChar(q_pos1);
            ref_pos++;
            c_pos1++;
            c_pos2++;
            q_pos1++;
            if (c2 == 'S') q_pos2++;
        } else if (c2 == 'M'){
            nucigar += 'M';
            dna +=  (*s2)[q_pos2];
            qualities += s2->qualityChar(q_pos2);
            ref_pos++;
            c_pos1++;
            c_pos2++;
            q_pos2++;
            if (c1 == 'S') q_pos1++;
        } else if (c1 == 'S'){
            ref_pos++;
            c_pos1++;
            c_pos2++;
            q_pos1++;
        } else {
            ref_pos++;
            c_pos1++;
            c_pos2++;
            q_pos2++;
        }
    } else if (c1 == 'I' || c2 == 'I'){
        if(c1 == 'I'){
            nucigar += 'I';
            dna += (*s1)[q_pos1];
            qualities += s1->qualityChar(q_pos1);
            c_pos1++;
            q_pos1++;
        } else {
            nucigar += 'I';
            dna +=  (*s2)[q_pos2];
            qualities += s2->qualityChar(q_pos2);
            c_pos2++;
            q_pos2++;
        }
    } else{
        assert(false);
    }
}



size_t AlignmentRecord::intersectionLength(const AlignmentRecord& ap) const {
	assert(single_end == ap.single_end);
	int left = max(getIntervalStart(), ap.getIntervalStart());
	int right = min(getIntervalEnd(), ap.getIntervalEnd()) + 1;
	return max(0, right-left);
}

size_t AlignmentRecord::internalSegmentIntersectionLength(const AlignmentRecord& ap) const {
	int left = max(getInsertStart(), ap.getInsertStart());
	int right = min(getInsertEnd(), ap.getInsertEnd()) + 1;
	return max(0, right-left);
}

int AlignmentRecord::getPhredSum1() const {
	return phred_sum1;
}

int AlignmentRecord::getPhredSum2() const {
	assert(!single_end);
	return phred_sum2;
}

double AlignmentRecord::getProbability() const {
	return probability;
}

unsigned int AlignmentRecord::getEnd1() const {
	return end1;
}

unsigned int AlignmentRecord::getEnd2() const {
	assert(!single_end);
	return end2;
}

std::string AlignmentRecord::getName() const {
	return name;
}

unsigned int AlignmentRecord::getStart1() const {
	return start1;
}

unsigned int AlignmentRecord::getStart2() const {
	assert(!single_end);
	return start2;
}

const std::vector<BamTools::CigarOp>& AlignmentRecord::getCigar1() const {
	return cigar1;
}

const std::vector<BamTools::CigarOp>& AlignmentRecord::getCigar2() const {
	assert(!single_end);
	return cigar2;
}

const ShortDnaSequence& AlignmentRecord::getSequence1() const {
	return sequence1;
}

const ShortDnaSequence& AlignmentRecord::getSequence2() const {
	assert(!single_end);
	return sequence2;
}

unsigned int AlignmentRecord::getIntervalStart() const {
	return start1;
}

unsigned int AlignmentRecord::getIntervalEnd() const {
	if (single_end) {
		return end1;
	} else {
		return end2;
	}
}

unsigned int AlignmentRecord::getInsertStart() const {
	assert(!single_end);
	return end1 + 1;
}

unsigned int AlignmentRecord::getInsertEnd() const {
	assert(!single_end);
	return start2 - 1;
}

unsigned int AlignmentRecord::getInsertLength() const {
	assert(!single_end);
	return start2 - (end1 + 1);
}

alignment_id_t AlignmentRecord::getID() const {
	return id;
}

void AlignmentRecord::setID(alignment_id_t id) {
	this->id = id;
}

bool AlignmentRecord::isSingleEnd() const {
	return single_end;
}

bool AlignmentRecord::isPairedEnd() const {
	return !single_end;
}

std::vector<std::string> AlignmentRecord::getReadNames() const {
    vector<string> rnames;
    for (int i : this->readNames) {
        rnames.push_back((*readNameMap)[i]);
    }
	return rnames;
}

const std::vector<char>& AlignmentRecord::getCigar1Unrolled() const {
	return this->cigar1_unrolled;
}
const std::vector<char>& AlignmentRecord::getCigar2Unrolled() const {
	return this->cigar2_unrolled;
}
int AlignmentRecord::getLengthInclDeletions1() const {
	return this->length_incl_deletions1;
}
int AlignmentRecord::getLengthInclDeletions2() const {
	return this->length_incl_deletions2;
}
int AlignmentRecord::getLengthInclLongDeletions1() const {
	return this->length_incl_longdeletions1;
}
int AlignmentRecord::getLengthInclLongDeletions2() const {
	return this->length_incl_longdeletions2;
}

double setProbabilities(std::deque<AlignmentRecord*>& reads) {
    double read_usage_ct = 0.0;
    double mean = 1.0 / reads.size();

    for(auto&& r : reads) {
        read_usage_ct += r->getReadCount();
    }

    if (not reads.empty()) {
        read_usage_ct = max(read_usage_ct, (double) reads[0]->readNameMap->size());
    }
    double stdev = 0.0;

    for (auto&& r : reads) {
        r->probability = r->getReadCount() / read_usage_ct;
        
        stdev += (r->probability - mean)*(r->probability - mean);
    }

    return sqrt(1.0 / (reads.size() - 1) * stdev);
}

void printReads(std::ostream& outfile, std::deque<AlignmentRecord*>& reads, int doc_haplotypes) {
    auto comp = [](AlignmentRecord* al1, AlignmentRecord* al2) { return al1->probability > al2->probability; };
    std::sort(reads.begin(), reads.end(), comp);

    outfile.precision(5);
    outfile << std::fixed;


    if (doc_haplotypes == 0){
        for (auto&& r : reads) {
            unsigned int abs_number_reads = r->getReadNames().size();
            outfile << ">" <<r->name;
            if (not r->single_end) outfile << "|paired";
            outfile << "|ht_freq:" << r->probability;
            outfile << "|start1:" << r->getStart1();
            outfile << "|end1:" << r->getEnd1();
            if (not r->single_end){
                outfile << "|start2:" << r->getStart2();
                outfile << "|end2:" << r->getEnd2();
            }
            outfile << "|#reads:" << abs_number_reads;
            outfile << endl;

            outfile << r->sequence1;
            if (not r->single_end) {
                for(unsigned int i = r->end1+1; i < r->start2; i++) {
                    outfile << "N";
                }
                outfile << r->sequence2;
            }
            outfile << endl;
        }
    } else if(doc_haplotypes == 5) {
        std::vector<std::string> names;
        for (auto&& r : reads) {
            names = r->getReadNames();
            int haplo1counter = 0;
            int haplo2counter = 0;
            int haplo3counter = 0;
            int haplo4counter = 0;
            int haplo5counter = 0;
            outfile << ">" << r->name;

            if(r->single_end){
                outfile << "|ht_freq:" << r->probability;
                outfile << "|start1:" << r->getStart1();
                outfile << "|end1:" << r->getEnd1();
            }
            else if(!r->single_end && (r->getEnd1()+1 < r->getStart2())){
                outfile << "|paired";
                outfile << "|ht_freq:" << r->probability;
                outfile << "|start1:" << r->getStart1();
                outfile << "|end1:" << r->getEnd1();
                outfile << "|start2:" << r->getStart2();
                outfile << "|end2:" << r->getEnd2();
            }
            else if (!r->single_end && (r->getEnd1()+1 == r->getStart2())){
                outfile << "|ht_freq:" << r->probability;
                outfile << "|start1:" << r->getStart1();
                outfile << "|end1:" << r->getEnd2();
            }
            for(auto& i: names){
                 if (i.find("mutant1") != std::string::npos){
                    haplo1counter++;
                 } else if (i.find("mutant2") != std::string::npos) {
                    haplo2counter++;
                 } else if (i.find("mutant3") != std::string::npos) {
                    haplo3counter++;
                 } else if (i.find("mutant4") != std::string::npos) {
                    haplo4counter++;
                 } else if (i.find("mutant5") != std::string::npos) {
                    haplo5counter++;
                 }
            }
            outfile << "|ht1:" << haplo1counter;
            outfile << "|ht2:" << haplo2counter;
            outfile << "|ht3:" << haplo3counter;
            outfile << "|ht4:" << haplo4counter;
            outfile << "|ht5:" << haplo5counter;
            outfile << endl;

            outfile << r->sequence1;

            if (not r->single_end) {
                for(unsigned int i = r->end1+1; i < r->start2; i++) {
                    outfile << "N";
                }
                outfile << r->sequence2;
            }
            outfile << endl;
        }
    } else if(doc_haplotypes == 2){
        std::vector<std::string> names;
        for (auto&& r : reads) {
            names = r->getReadNames();
            int haplo1counter = 0;
            int haplo2counter = 0;
            outfile << ">" << r->name;

            if(r->single_end){
                outfile << "|ht_freq:" << r->probability;
                outfile << "|start1:" << r->getStart1();
                outfile << "|end1:" << r->getEnd1();
            }
            else if(!r->single_end && (r->getEnd1()+1 < r->getStart2())){
                outfile << "|paired";
                outfile << "|ht_freq:" << r->probability;
                outfile << "|start1:" << r->getStart1();
                outfile << "|end1:" << r->getEnd1();
                outfile << "|start2:" << r->getStart2();
                outfile << "|end2:" << r->getEnd2();
            }
            else if (!r->single_end && (r->getEnd1()+1 == r->getStart2())){
                outfile << "|ht_freq:" << r->probability;
                outfile << "|start1:" << r->getStart1();
                outfile << "|end1:" << r->getEnd2();
            }
            for(auto& i: names){
                 if (i.find("normal") != std::string::npos){
                    haplo1counter++;
                 } else if (i.find("mutant") != std::string::npos) {
                    haplo2counter++;
                 }
            }
            outfile << "|ht1:" << haplo1counter;
            outfile << "|ht2:" << haplo2counter;
            outfile << endl;

            outfile << r->sequence1;

            if (not r->single_end) {
                for(unsigned int i = r->end1+1; i < r->start2; i++) {
                    outfile << "N";
                }
                outfile << r->sequence2;
            }
            outfile << endl;
        }
    }
}

void printGFF(std::ostream& output, std::deque<AlignmentRecord*>& reads){
    assert(false);
}

void printBAM(std::ostream& output, std::string filename, std::deque<AlignmentRecord*>& reads ,BamTools::SamHeader& header, BamTools::RefVector& references){
    BamTools::BamAlignment al;
    //Debugging: read in bam file of singletons to member variables:
    /*
    BamTools::BamReader bamreader;
    if (not bamreader.Open("/MMCI/TM/structvar/work/hivhaplo/output/KMT/singletons.bam")) {
        cerr << bamreader.GetFilename() << endl;
        throw std::runtime_error("Couldn't open Bamfile");
    }
    int counter = 0;
    while (bamreader.GetNextAlignment(al)){
        cout << al.IsDuplicate() << endl;
        cout << al.IsFailedQC() << endl;
        cout << al.IsFirstMate() << endl;
        cout << al.IsMapped() << endl;
        cout << al.IsMateMapped() << endl;
        cout << al.IsMateReverseStrand() << endl;
        cout << al.IsPaired() << endl;
        cout << al.IsPrimaryAlignment() << endl;
        cout << al.IsProperPair() << endl;
        cout << al.IsReverseStrand() << endl;
        cout << al.IsSecondMate() << endl;
        cout << al.Length << endl;
        cout << al.QueryBases << endl;
        cout << al.AlignedBases << endl;
        cout << al.Qualities << endl;
        cout << al.TagData << endl;
        cout << al.RefID << endl;
        cout << al.Position << endl;
        cout << al.Bin << endl;
        cout << al.MapQuality << endl;
        cout << al.AlignmentFlag << endl;
        cout << al.MateRefID << endl;
        cout << al.MatePosition << endl;
        cout << al.InsertSize << endl;
        cout << al.Filename << endl;
        counter++;
        if (counter == 3){
            break;
        }
    }
    */
    BamTools::BamWriter writer;
    //get Header and get References
    if ( !writer.Open(filename, header, references) ) {
        cerr << "Could not open output BAM file" << endl;
        throw std::runtime_error("Couldn't open output Bamfile");
    return;
    }
    /* for a better overview this comment contains information about the members of a BamTools::BamAlignment
        std::string Name;               // read name
        int32_t     Length;             // length of query sequence
        std::string QueryBases;         // 'original' sequence (contained in BAM file)
        std::string AlignedBases;       // 'aligned' sequence (QueryBases plus deletion, padding, clipping chars)
        std::string Qualities;          // FASTQ qualities (ASCII characters, not numeric values)
        std::string TagData;            // tag data (use provided methods to query/modify)
        int32_t     RefID;              // ID number for reference sequence
        int32_t     Position;           // position (0-based) where alignment starts
        uint16_t    Bin;                // BAM (standard) index bin number for this alignment
        uint16_t    MapQuality;         // mapping quality score
        uint32_t    AlignmentFlag;      // alignment bit-flag (use provided methods to query/modify)
        std::vector<CigarOp> CigarData; // CIGAR operations for this alignment
        int32_t     MateRefID;          // ID number for reference sequence where alignment's mate was aligned
        int32_t     MatePosition;       // position (0-based) where alignment's mate starts
        int32_t     InsertSize;         // mate-pair insert size
        std::string Filename;           // name of BAM file which this alignment comes from

    BamAlignment::BamAlignment(void)
    : Length(0)
    , RefID(-1)
    , Position(-1)
    , Bin(0)
    , MapQuality(0)
    , AlignmentFlag(0)
    , MateRefID(-1)
    , MatePosition(-1)
    , InsertSize(0)
    */
    // iterate through all alignments and write them to output
    for (auto&& r : reads){
        //set members of al
        if(r->single_end){
            al.Name = r->getName();
            al.Length = r->getSequence1().size();
            al.QueryBases = r->getSequence1().toString();
            //no aligned bases
            al.Qualities = r->getSequence1().qualityString();
            //no tag data
            al.RefID = 0;
            al.Position = r->getStart1()-1;
            //no bin, map quality. alignment flag is set extra
            al.MateRefID = 0;
            al.MatePosition = r->getStart1()-1;
            al.InsertSize = 0;
            al.CigarData = r->getCigar1();
            al.Filename = filename;
            //set flags
            al.SetIsDuplicate(false);
            al.SetIsFailedQC(false);
            al.SetIsFirstMate(true);
            al.SetIsMapped(true);
            al.SetIsMateMapped(false);
            al.SetIsMateReverseStrand(false);
            al.SetIsPaired(true);
            al.SetIsPrimaryAlignment(true);
            al.SetIsProperPair(false);
            al.SetIsReverseStrand(false);
            al.SetIsSecondMate(false);
            writer.SaveAlignment(al);
        } else {
            al.Name = r->getName();
            al.Length = r->getSequence1().size();
            al.QueryBases = r->getSequence1().toString();
            //no aligned bases
            al.Qualities = r->getSequence1().qualityString();
            //no tag data
            //given the case that reads were mapped against more than one reference, al.RefID has to be modified
            al.RefID = 0;
            al.Position = r->getStart1()-1;
            //no bin, map quality. alignment flag is set extra
            al.MateRefID = 0;
            al.CigarData = r->getCigar1();
            al.MatePosition = r->getStart2()-1;
            al.InsertSize =r->getEnd2()-r->getStart1()+1;
            al.Filename = filename;
            //set flags
            al.SetIsDuplicate(false);
            al.SetIsFailedQC(false);
            al.SetIsFirstMate(true);
            al.SetIsMapped(true);
            al.SetIsMateMapped(true);
            al.SetIsMateReverseStrand(false);
            al.SetIsPaired(true);
            al.SetIsPrimaryAlignment(true);
            al.SetIsProperPair(true);
            al.SetIsReverseStrand(false);
            al.SetIsSecondMate(false);
            writer.SaveAlignment(al);
            al.Name = r->getName();
            al.Length = r->getSequence2().size();
            al.QueryBases = r->getSequence2().toString();
            //no aligned bases
            al.Qualities = r->getSequence2().qualityString();
            //no tag data
            //given the case that reads were mapped against more than one reference, al.RefID has to be modified
            al.RefID = 0;
            al.Position = r->getStart2()-1;
            //no bin, map quality. alignment flag is set extra.
            al.CigarData = r->getCigar2();
            al.MatePosition = r->getStart1()-1;
            al.InsertSize =(-1)*(r->getEnd2()-r->getStart1()+1);
            al.Filename = filename;
            //set flags
            al.SetIsDuplicate(false);
            al.SetIsFailedQC(false);
            al.SetIsFirstMate(false);
            al.SetIsMapped(true);
            al.SetIsMateMapped(true);
            al.SetIsMateReverseStrand(false);
            al.SetIsPaired(true);
            al.SetIsPrimaryAlignment(true);
            al.SetIsProperPair(true);
            al.SetIsReverseStrand(false);
            al.SetIsSecondMate(true);
            writer.SaveAlignment(al);
        }
        //write to output
    }
    writer.Close();
}

