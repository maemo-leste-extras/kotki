#include "translation_model.h"

#include "kotki/batch.h"
#include "kotki/byte_array_util.h"
#include "kotki/cache.h"
#include "marian-lite/common/logging.h"
#include "marian-lite/data/corpus.h"
#include "marian-lite/data/text_input.h"
#include "kotki/parser.h"
#include "marian-lite/translator/beam_search.h"

namespace marian {
namespace bergamot {

std::atomic<size_t> TranslationModel::modelCounter_ = 0;

TranslationModel::TranslationModel(const Config &options, MemoryBundle &&memory /*=MemoryBundle{}*/,
                                   size_t replicas /*=1*/)
    : modelId_(modelCounter_++),
      options_(options),
      memory_(std::move(memory)),
      vocabs_(options, std::move(memory_.vocabs)),
      textProcessor_(options, vocabs_, std::move(memory_.ssplitPrefixFile)),
      batchingPool_(options),
      qualityEstimator_(createQualityEstimator(getQualityEstimatorModel(memory, options))) {
  ABORT_IF(replicas == 0, "At least one replica needs to be created.");
  backend_.resize(replicas);

  // Try to load shortlist from memory-bundle. If not available, try to load from options_;

  int srcIdx = 0, trgIdx = 1;
  // vocabs_->sources().front() is invoked as we currently only support one source vocab
  bool shared_vcb = (vocabs_.sources().front() == vocabs_.target());

  if (memory_.shortlist.size() > 0 && memory_.shortlist.begin() != nullptr) {
    bool check = options_->get<bool>("check-bytearray", false);
    shortlistGenerator_ = New<data::BinaryShortlistGenerator>(memory_.shortlist.begin(), memory_.shortlist.size(),
                                                              vocabs_.sources().front(), vocabs_.target(), srcIdx,
                                                              trgIdx, shared_vcb, check);
  } else if (options_->hasAndNotEmpty("shortlist")) {
    // Changed to BinaryShortlistGenerator to enable loading binary shortlist file
    // This class also supports text shortlist file
    shortlistGenerator_ = New<data::BinaryShortlistGenerator>(options_, vocabs_.sources().front(), vocabs_.target(),
                                                              srcIdx, trgIdx, shared_vcb);
  } else {
    // In this case, the loadpath does not load shortlist.
    shortlistGenerator_ = nullptr;
  }
}

void TranslationModel::loadBackend(size_t idx) {
  auto &graph = backend_[idx].graph;
  auto &scorerEnsemble = backend_[idx].scorerEnsemble;

  marian::DeviceId device_(idx, DeviceType::cpu);
  graph = New<ExpressionGraph>(/*inference=*/true);  // set the graph to be inference only
  auto prec = options_->get<std::vector<std::string>>("precision", {"float32"});
  graph->setDefaultElementType(typeFromString(prec[0]));
  graph->setDevice(device_);
  graph->getBackend()->configureDevice(options_);

#ifdef __arm__
  graph->reserveWorkspaceMB(128);
#elif __x86_64__
  graph->reserveWorkspaceMB(512);
#else
  throw std::runtime_error("unknown arch");
#endif

  // Marian Model: Load from memoryBundle or shortList
  if (memory_.model.size() > 0 &&
      memory_.model.begin() !=
          nullptr) {  // If we have provided a byte array that contains the model memory, we can initialise the
                      // model from there, as opposed to from reading in the config file
    ABORT_IF((uintptr_t)memory_.model.begin() % 256 != 0,
             "The provided memory is not aligned to 256 bytes and will crash when vector instructions are used on it.");
    if (options_->get<bool>("check-bytearray", false)) {
      ABORT_IF(!validateBinaryModel(memory_.model, memory_.model.size()),
               "The binary file is invalid. Incomplete or corrupted download?");
    }
    const std::vector<const void *> container = {
        memory_.model.begin()};  // Marian supports multiple models initialised in this manner hence std::vector.
                                 // However we will only ever use 1 during decoding.
    scorerEnsemble = createScorers(options_, container);
  } else {
    scorerEnsemble = createScorers(options_);
  }
  for (auto scorer : scorerEnsemble) {
    scorer->init(graph);
    if (shortlistGenerator_) {
      scorer->setShortlistGenerator(shortlistGenerator_);
    }
  }
  graph->forward();
}

// Make request process is shared between Async and Blocking workflow of translating.
Ptr<Request> TranslationModel::makeRequest(std::string &&source, std::optional<TranslationCache> &cache) {
  Segments segments;
  AnnotatedText annotatedSource;

  textProcessor_.process(std::move(source), annotatedSource, segments);
  ResponseBuilder responseBuilder(std::move(annotatedSource), vocabs_, *qualityEstimator_);

  Ptr<Request> request =
      New<Request>(/*model=*/*this, std::move(segments), std::move(responseBuilder), cache);
  return request;
}

Ptr<marian::data::CorpusBatch> TranslationModel::convertToMarianBatch(Batch &batch) {
  std::vector<data::SentenceTuple> batchVector;
  auto &sentences = batch.sentences();

  size_t batchSequenceNumber{0};
  for (auto &sentence : sentences) {
    data::SentenceTuple sentence_tuple(batchSequenceNumber);
    Segment segment = sentence.getUnderlyingSegment();
    sentence_tuple.push_back(segment);
    batchVector.push_back(sentence_tuple);

    ++batchSequenceNumber;
  }

  // Usually one would expect inputs to be [B x T], where B = batch-size and T = max seq-len among the sentences in the
  // batch. However, marian's library supports multi-source and ensembling through different source-vocabulary but same
  // target vocabulary. This means the inputs are 3 dimensional when converted into marian's library formatted batches.
  //
  // Consequently B x T projects to N x B x T, where N = ensemble size. This adaptation does not fully force the idea of
  // N = 1 (the code remains general, but N iterates only from 0-1 in the nested loop).

  size_t batchSize = batchVector.size();

  std::vector<size_t> sentenceIds;
  std::vector<int> maxDims;

  for (auto &example : batchVector) {
    if (maxDims.size() < example.size()) {
      maxDims.resize(example.size(), 0);
    }
    for (size_t i = 0; i < example.size(); ++i) {
      if (example[i].size() > static_cast<size_t>(maxDims[i])) {
        maxDims[i] = static_cast<int>(example[i].size());
      }
    }
    sentenceIds.push_back(example.getId());
  }

  using SubBatch = marian::data::SubBatch;
  std::vector<Ptr<SubBatch>> subBatches;
  for (size_t j = 0; j < maxDims.size(); ++j) {
    subBatches.emplace_back(New<SubBatch>(batchSize, maxDims[j], vocabs_.sources().at(j)));
  }

  std::vector<size_t> words(maxDims.size(), 0);
  for (size_t i = 0; i < batchSize; ++i) {
    for (size_t j = 0; j < maxDims.size(); ++j) {
      for (size_t k = 0; k < batchVector[i][j].size(); ++k) {
        subBatches[j]->data()[k * batchSize + i] = batchVector[i][j][k];
        subBatches[j]->mask()[k * batchSize + i] = 1.f;
        words[j]++;
      }
    }
  }

  for (size_t j = 0; j < maxDims.size(); ++j) {
    subBatches[j]->setWords(words[j]);
  }

  using CorpusBatch = marian::data::CorpusBatch;
  Ptr<CorpusBatch> corpusBatch = New<CorpusBatch>(subBatches);
  corpusBatch->setSentenceIds(sentenceIds);
  return corpusBatch;
}

void TranslationModel::translateBatch(size_t deviceId, Batch &batch) {
  auto &backend = backend_[deviceId];

  if (!backend.initialized) {
    loadBackend(deviceId);
    backend.initialized = true;
  }

  BeamSearch search(options_, backend.scorerEnsemble, vocabs_.target());
  Histories histories = search.search(backend.graph, convertToMarianBatch(batch));
  batch.completeBatch(histories);
}

}  // namespace bergamot
}  // namespace marian
