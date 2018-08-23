// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include "infer.h"

using namespace ISLE;

// Run ./ISLEInfer
// Output: list of <doc_id> <topic id> <wt>  (small wt entries will be dropped)
int main(int argv, char**argc)
{
    if (argv != 11) {
        std::cout << "Incorrect usage of ISLEInfer. Use: \n"
            << "inferFromFile <sparse_model_file> <infer_file> <output_dir> "
            << "<num_topics> <vocab_size> <num_docs_in_infer_file> "
            << "<nnzs_in_infer_file> <nnzs_in_sparse_model_file> "
            << "<iters>[0 for default]  "
            << "Lifschitz_constant_guess>[0 for default]" << std::endl;
        exit(-1);
    }
    const std::string sparse_model_file = std::string(argc[1]);
    const std::string infer_file = std::string(argc[2]);
    const std::string output_dir = std::string(argc[3]);

    const doc_id_t num_topics = atol(argc[4]);
    const word_id_t vocab_size = atol(argc[5]);
    const doc_id_t num_docs = atol(argc[6]);
    const offset_t max_entries = atol(argc[7]);
    const offset_t M_hat_catch_sparse_entries = atol(argc[8]);

    int iters = atol(argc[9]);
    if (iters == 0) iters = INFER_ITERS_DEFAULT;
    FPTYPE Lfguess = atof(argc[10]);
    if (Lfguess == 0.0) Lfguess = INFER_LF_DEAFULT;

    std::cout << "Loading sparse model file: " << sparse_model_file << std::endl;
    FPTYPE *model_by_word = new FPTYPE[vocab_size * num_topics];
    memset(model_by_word, 0, sizeof(FPTYPE)*vocab_size*num_topics);
    load_model_from_sparse_file(model_by_word, num_topics, vocab_size, sparse_model_file, 1);


    std::cout << "Loading data from inference file: " << infer_file << std::endl;
    std::vector<DocWordEntry<count_t> > entries;
    DocWordEntriesReader reader(entries);
    reader.read_from_file(infer_file, max_entries);
    auto infer_data = new SparseMatrix<FPTYPE>(vocab_size, num_docs);
    std::sort(			// Sort by doc first, and word second.
        entries.begin(), entries.end(),
        [](const auto& l, const auto& r)
    {return (l.doc < r.doc) || (l.doc == r.doc && l.word < r.word); });
    entries.erase(			// Remove duplicates
        std::unique(entries.begin(), entries.end(),
            [](const auto& l, const auto& r) {return l.doc == r.doc && l.word == r.word; }),
        entries.end());
    infer_data->populate_CSC(entries);
    infer_data->normalize_docs(true, true);

    auto llhs = new std::pair<FPTYPE, FPTYPE>[num_docs];

    // Turn the following flag on for parallel inference that works block by block
#ifdef PARALLEL_INFERENCE
    doc_id_t doc_block_size = 100000;
    int64_t num_blocks = divide_round_up(num_docs, doc_block_size);
    auto nconverged = new doc_id_t[num_blocks];

    pfor(int64_t block = 0; block < num_blocks; ++block) {
        nconverged[block] = 0;
        std::cout << "Creating inference engine" << std::endl;
        ISLEInfer infer(model_by_word, infer_data, num_topics, vocab_size, num_docs);
        MMappedOutput out(concat_file_path(output_dir,
            std::string("inferred_weights_iters_") + std::to_string(iters)
            + std::string("_Lf_") + std::to_string(Lfguess))
            + std::string("_block_") + std::to_string(block));
        FPTYPE* wts = new FPTYPE[num_topics];
        for (doc_id_t doc = block*doc_block_size; doc < (block + 1)*doc_block_size && doc < num_docs; ++doc) {
            if (doc % 10000 == 9999)
                std::cout << "docs inferred: ["
                << (((int64_t)doc - (int64_t)10000) > (int64_t)(block*doc_block_size) ? ((int64_t)doc - (int64_t)10000) : block*doc_block_size)
                << ", " << doc << "]" << std::endl;

            llhs[doc] = infer.infer_doc_in_file(doc, wts, iters, Lfguess);
            if (llhs[doc].first != 0.0)
                nconverged[block]++;
            else std::cout << "Doc: " << doc << "failed to converge" << std::endl;
            for (doc_id_t topic = 0; topic < num_topics; ++topic)
                out.concat_float(llhs[doc].first == 0.0 ? 1.0 / (FPTYPE)num_topics : wts[topic], '\t', 1, 8);
            out.add_endline();
        }
        out.flush_and_close();

        delete[] wts;
    }
    auto nconvergedall = std::accumulate(nconverged, nconverged + num_blocks, 0.0);
    delete[] nconverged;
#else
    doc_id_t nconverged = 0;

    std::cout << "Creating inference engine" << std::endl;
    ISLEInfer infer(model_by_word, infer_data, num_topics, vocab_size, num_docs);
    MMappedOutput out(concat_file_path(output_dir,
        std::string("inferred_weights_iters_") + std::to_string(iters)
        + std::string("_Lf_") + std::to_string(Lfguess)));
   
    FPTYPE* wts = new FPTYPE[num_topics];
    for (doc_id_t doc = 0; doc < num_docs; ++doc) {
        if (doc % 10000 == 9999)
            std::cout << "docs inferred: " << doc << std::endl;

        llhs[doc] = infer.infer_doc_in_file(doc, wts, iters, Lfguess);
        if (llhs[doc].first != 0.0)
            nconverged[block]++;
        else std::cout << "Doc: " << doc << "failed to converge" << std::endl;
        for (doc_id_t topic = 0; topic < num_topics; ++topic)
            out.concat_float(llhs[doc].first == 0.0 ? 1.0 / (FPTYPE)num_topics : wts[topic], '\t', 1, 8);
        out.add_endline();
    }
    out.flush_and_close();
    delete[] wts;

    auto nconvergedall = nconverged;
#endif

    std::cout << "Number of docs for which inference converged: " << nconvergedall
        << " (of " << num_docs << ")" << std::endl;

    std::pair<FPTYPE,FPTYPE> sum_llhs;
    sum_llhs.first = 0.0;
    sum_llhs.second = 0.0;

     
    for (doc_id_t doc = 0; doc < num_docs; doc++){
        sum_llhs.first += llhs[doc].first;
        sum_llhs.second += llhs[doc].second;
    }

    std::cout << "Avg LLH per document for converged docs: "
      << ((FPTYPE)num_docs/nconvergedall) * sum_llhs.first / nconvergedall << std::endl;

    std::cout << "Avg LLH per word: "
      << sum_llhs.second / max_entries << std::endl;

    delete[] llhs;
    delete infer_data;
    delete[] model_by_word;

    return 0;
}
