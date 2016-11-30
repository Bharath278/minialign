/*
 * mhap.cc
 *
 *  Created on: May 29, 2016
 *      Author: isovic
 */

#include <map>
#include <algorithm>
#include "libs/edlib.h"
#include "mhap.h"
#include <omp.h>
#include <vector>
#include <iostream>

int ParseMHAP(const std::string &mhap_path, std::vector<OverlapLine> &ret_overlaps) {
  ret_overlaps.clear();

  if (mhap_path != "-") {
    std::ifstream infile(mhap_path);
    std::string line;
    while (std::getline(infile, line))
    {
      std::istringstream iss(line);
      OverlapLine mhap_line;
      if (!mhap_line.ParseMHAP(line)) {
        ret_overlaps.push_back(mhap_line);
      }
    }
    infile.close();

  } else {
    std::string line;
    while (std::getline(std::cin, line))
    {
      if (line.size() == 0) { break; }

      std::istringstream iss(line);
      OverlapLine mhap_line;
      if (!mhap_line.ParseMHAP(line)) {
        ret_overlaps.push_back(mhap_line);
      }
    }

  }
  return 0;
}

int ParsePAF(const std::string &paf_path, const std::map<std::string, int64_t> &qname_to_ids, std::vector<OverlapLine> &ret_overlaps) {
  ret_overlaps.clear();

  if (paf_path != "-") {
    std::ifstream infile(paf_path);
    std::string line;
    while (std::getline(infile, line))
    {
      std::istringstream iss(line);
      OverlapLine paf_line;
      if (!paf_line.ParsePAF(line, qname_to_ids)) {
        ret_overlaps.push_back(paf_line);
      }
    }
    infile.close();

  } else {
    std::string line;
    while (std::getline(std::cin, line))
    {
      if (line.size() == 0) { break; }

      std::istringstream iss(line);
      OverlapLine paf_line;
      if (!paf_line.ParsePAF(line, qname_to_ids)) {
        ret_overlaps.push_back(paf_line);
      }
    }

  }

  return 0;
}

int AlignOverlaps(const SequenceFile &refs, const SequenceFile &reads, const std::vector<OverlapLine> &overlaps, int32_t num_threads, SequenceFile &aligned, bool verbose_debug, bool write_right_away) {
  // Don't store alignments internally.
  if (write_right_away == false) {
    aligned.Clear();

    // Generate the SAM file header, for debugging.
    std::vector<std::string> sam_header;
    sam_header.push_back(FormatString("@HD\tVN:1.0\tSO:unknown"));
    for (int64_t i=0; i<refs.get_sequences().size(); i++) {
      const SingleSequence *ref = refs.get_sequences()[i];
      sam_header.push_back(FormatString("@SQ\tSN:%s\tLN:%ld", ref->get_header(), ref->get_data_length()));
    }
    aligned.set_file_header(sam_header);
  }

  int64_t num_skipped_overlaps = 0;

  #pragma omp parallel for num_threads(num_threads) shared(aligned) schedule(dynamic, 1)
  for (int64_t i=0; i<overlaps.size(); i++) {
    int32_t thread_id = omp_get_thread_num();

    if (verbose_debug == true && thread_id == 0) {
      LOG_ALL("\rAligning overlap: %ld / %ld (%.2f\%), skipped %ld / %ld (%.2f\%)", i, overlaps.size(), 100.0f*((float) (i + 1)) / ((float) overlaps.size()), num_skipped_overlaps, overlaps.size(), 100.0f*((float) (num_skipped_overlaps)) / ((float) overlaps.size()));
      fflush(stderr);
    }

    auto &mhap = overlaps[i];
    OverlapLine omhap = mhap;

    // Get the read.
    const SingleSequence* read = reads.get_sequences()[omhap.Aid - 1];
    // Get the reference. It could be reversed.
    const SingleSequence* ref = NULL;
    ref = refs.get_sequences()[omhap.Bid - 1];

    std::string ref_name(ref->get_header(), ref->get_header_length());

    SingleSequence *seq = new SingleSequence();
    seq->InitAllFromAscii((char *) read->get_header(), read->get_header_length(),
                          (int8_t *) read->get_data(), (int8_t *) read->get_quality(), read->get_data_length(),
                          aligned.get_sequences().size(), aligned.get_sequences().size());
    if (omhap.Brev) {
      seq->ReverseComplement();
      omhap.Astart = mhap.Alen - mhap.Aend;
      omhap.Aend = mhap.Alen - mhap.Astart - 1;
    }

    int64_t aln_start = 0, aln_end = 0, editdist = 0;
    std::vector<unsigned char> alignment;
    int rcaln = EdlibNWWrapper(seq->get_data() + omhap.Astart, (omhap.Aend - omhap.Astart),
                            ref->get_data() + omhap.Bstart, (omhap.Bend - omhap.Bstart),
                            &aln_start, &aln_end, &editdist, alignment);

    if (!rcaln) {
      char *cigar_cstring = NULL;
      int rccig = edlibAlignmentToCigar(&alignment[0], alignment.size(), EDLIB_CIGAR_EXTENDED, &cigar_cstring);

      std::string cigar_string(cigar_cstring);

      SequenceAlignment aln;
      aln.SetCigarFromString(cigar_string);
      aln.set_pos(omhap.Bstart + aln_start + 1);
      aln.set_flag((int32_t) (16 * omhap.Brev));     // 0 if fwd and 16 if rev.
      aln.set_mapq(40);
      aln.set_rname(ref_name);

      // Remove insertions at front.
      for (int32_t j=0; j<aln.cigar().size(); j++) {
        if (aln.cigar()[j].count == 0) { continue; }
        if (aln.cigar()[j].op != 'D') { break; }
        aln.cigar()[j].count = 0;
      }
      for (int32_t j=0; j<aln.cigar().size(); j++) {
        if (aln.cigar()[j].count == 0) { continue; }
        if (aln.cigar()[j].op != 'I') { break; }
        aln.cigar()[j].op = 'S';
      }
      // Remove insertions at back.
      for (int32_t j=(aln.cigar().size()-1); j>=0; j--) {
        if (aln.cigar()[j].count == 0) { continue; }
        if (aln.cigar()[j].op != 'D') { break; }
        aln.cigar()[j].count = 0;
      }
      for (int32_t j=(aln.cigar().size()-1); j>=0; j--) {
        if (aln.cigar()[j].count == 0) { continue; }
        if (aln.cigar()[j].op != 'I') { break; }
        aln.cigar()[j].op = 'S';
      }

      // If the overlap does not cover the entire read (and most likely it does not).
      if (omhap.Astart > 0) {
        if (aln.cigar().size() > 0 && aln.cigar().front().op == 'S') { aln.cigar().front().count += omhap.Astart; }
        else { CigarOp new_op; new_op.op = 'S'; new_op.count = omhap.Astart; aln.cigar().insert(aln.cigar().begin(), new_op); }
      }
      if ((omhap.Aend) < (omhap.Alen)) {
        if (aln.cigar().size() > 0 && aln.cigar().back().op == 'S') { aln.cigar().back().count += (omhap.Alen - omhap.Aend); }
        else { CigarOp new_op; new_op.op = 'S'; new_op.count = (omhap.Alen - omhap.Aend); aln.cigar().insert(aln.cigar().end(), new_op); }
      }

      aln.RecalcCigarPositions();

      int64_t m=0, x=0, eq=0, ins=0, del=0;
      for (int32_t j=0; j<aln.cigar().size(); j++) {
        if (aln.cigar()[j].op == 'M') { m += aln.cigar()[j].count; }
        if (aln.cigar()[j].op == '=') { eq += aln.cigar()[j].count; }
        if (aln.cigar()[j].op == 'X') { x += aln.cigar()[j].count; }
        if (aln.cigar()[j].op == 'I') { ins += aln.cigar()[j].count; }
        if (aln.cigar()[j].op == 'D') { del += aln.cigar()[j].count; }
      }
      aln.optional().push_back(FormatString("NM:i:%d\tX1:Z:equal=%ld_x=%ld_ins=%ld_del=%ld", editdist, eq, x, ins, del));

      seq->InitAlignment(aln);

      if (write_right_away == false) {
        #pragma omp critical
        aligned.AddSequence(seq, true);
      } else {
        std::string sam_line = seq->MakeSAMLine();
        #pragma omp critical
        {
          fprintf (stdout, "%s\n", sam_line.c_str());
          fflush(stdout);
        }
      }

    } else {
      if (seq) {
        delete seq;
        seq = NULL;
      }
      num_skipped_overlaps += 1;
    }

  }

  if (verbose_debug == true) {
    fprintf (stderr, "\n");
    fflush(stderr);
  }

  return 0;
}

int64_t CalculateReconstructedLength(unsigned char *alignment, int alignmentLength) {
  if (alignment == NULL || alignmentLength == 0)
      return 0;

  int64_t length = 0;
  std::stringstream ss;

  for (int i=0; i<alignmentLength; i++) {
    if (alignment[i] == EDLIB_EDOP_MATCH || alignment[i] == EDLIB_EDOP_MISMATCH || alignment[i] == EDLIB_EDOP_DELETE)
      length += 1;
  }

  return length;
}

int EdlibNWWrapper(const int8_t *read_data, int64_t read_length,
                   const int8_t *reference_data, int64_t reference_length,
                   int64_t* ret_alignment_position_start, int64_t *ret_alignment_position_end,
                   int64_t *ret_edit_distance, std::vector<unsigned char> &ret_alignment) {

  if (read_data == NULL || reference_data == NULL || read_length <= 0 || reference_length <= 0)
    return 1;

  int alphabet_length = 128;
  int score = 0;
  unsigned char* alignment = NULL;
  int alignment_length = 0;

  int *positions = NULL;
  int num_positions = 0;
  int *start_locations = NULL;
  int found_k = 0;

  int myers_return_code = edlibCalcEditDistance((const unsigned char *) read_data, read_length,
                        (const unsigned char *) reference_data, reference_length,
                        alphabet_length, -1, EDLIB_MODE_NW, true, true, &score, &positions, &start_locations, &num_positions,
                        &alignment, &alignment_length, &found_k);

  if (myers_return_code == EDLIB_STATUS_ERROR || num_positions == 0 || alignment_length == 0) {
    return EDLIB_STATUS_ERROR;
  }

  int64_t reconstructed_length = CalculateReconstructedLength(alignment, alignment_length);

  *ret_alignment_position_start = positions[0] - (reconstructed_length - 1);
  *ret_alignment_position_end = positions[0];
  *ret_edit_distance = (int64_t) score;
  ret_alignment.assign(alignment, (alignment + alignment_length));

  if (positions)
    free(positions);

  if (start_locations)
    free(start_locations);

  if (alignment)
      free(alignment);

  return 0;
}
