#include "scorer.h"

#include <unistd.h>
#include <iostream>

#include "lm/config.hh"
#include "lm/model.hh"
#include "lm/state.hh"
#include "util/string_piece.hh"
#include "util/tokenize_piece.hh"

#include "decoder_utils.h"

using namespace lm::ngram;

Scorer::Scorer(double alpha, double beta, const std::string& lm_path) {
  this->alpha = alpha;
  this->beta = beta;
  _is_character_based = true;
  _language_model = nullptr;
  dictionary = nullptr;
  _max_order = 0;
  _SPACE_ID = -1;
  // load language model
  load_LM(lm_path.c_str());
}

Scorer::~Scorer() {
  if (_language_model != nullptr)
    delete static_cast<lm::base::Model*>(_language_model);
  if (dictionary != nullptr) delete static_cast<fst::StdVectorFst*>(dictionary);
}

void Scorer::load_LM(const char* filename) {
  if (access(filename, F_OK) != 0) {
    std::cerr << "Invalid language model file !!!" << std::endl;
    exit(1);
  }
  RetriveStrEnumerateVocab enumerate;
  lm::ngram::Config config;
  config.enumerate_vocab = &enumerate;
  _language_model = lm::ngram::LoadVirtual(filename, config);
  _max_order = static_cast<lm::base::Model*>(_language_model)->Order();
  _vocabulary = enumerate.vocabulary;
  for (size_t i = 0; i < _vocabulary.size(); ++i) {
    if (_is_character_based && _vocabulary[i] != UNK_TOKEN &&
        _vocabulary[i] != START_TOKEN && _vocabulary[i] != END_TOKEN &&
        get_utf8_str_len(enumerate.vocabulary[i]) > 1) {
      _is_character_based = false;
    }
  }
}

double Scorer::get_log_cond_prob(const std::vector<std::string>& words) {
  lm::base::Model* model = static_cast<lm::base::Model*>(_language_model);
  double cond_prob;
  lm::ngram::State state, tmp_state, out_state;
  // avoid to inserting <s> in begin
  model->NullContextWrite(&state);
  for (size_t i = 0; i < words.size(); ++i) {
    lm::WordIndex word_index = model->BaseVocabulary().Index(words[i]);
    // encounter OOV
    if (word_index == 0) {
      return OOV_SCORE;
    }
    cond_prob = model->BaseScore(&state, word_index, &out_state);
    tmp_state = state;
    state = out_state;
    out_state = tmp_state;
  }
  // return  log10 prob
  return cond_prob;
}

double Scorer::get_sent_log_prob(const std::vector<std::string>& words) {
  std::vector<std::string> sentence;
  if (words.size() == 0) {
    for (size_t i = 0; i < _max_order; ++i) {
      sentence.push_back(START_TOKEN);
    }
  } else {
    for (size_t i = 0; i < _max_order - 1; ++i) {
      sentence.push_back(START_TOKEN);
    }
    sentence.insert(sentence.end(), words.begin(), words.end());
  }
  sentence.push_back(END_TOKEN);
  return get_log_prob(sentence);
}

double Scorer::get_log_prob(const std::vector<std::string>& words) {
  assert(words.size() > _max_order);
  double score = 0.0;
  for (size_t i = 0; i < words.size() - _max_order + 1; ++i) {
    std::vector<std::string> ngram(words.begin() + i,
                                   words.begin() + i + _max_order);
    score += get_log_cond_prob(ngram);
  }
  return score;
}

void Scorer::reset_params(float alpha, float beta) {
  this->alpha = alpha;
  this->beta = beta;
}

std::string Scorer::vec2str(const std::vector<int>& input) {
  std::string word;
  for (auto ind : input) {
    word += _char_list[ind];
  }
  return word;
}

std::vector<std::string> Scorer::split_labels(const std::vector<int>& labels) {
  if (labels.empty()) return {};

  std::string s = vec2str(labels);
  std::vector<std::string> words;
  if (_is_character_based) {
    words = split_utf8_str(s);
  } else {
    words = split_str(s, " ");
  }
  return words;
}

void Scorer::set_char_map(const std::vector<std::string>& char_list) {
  _char_list = char_list;
  _char_map.clear();

  for (unsigned int i = 0; i < _char_list.size(); i++) {
    if (_char_list[i] == " ") {
      _SPACE_ID = i;
      _char_map[' '] = i;
    } else if (_char_list[i].size() == 1) {
      _char_map[_char_list[i][0]] = i;
    }
  }
}

std::vector<std::string> Scorer::make_ngram(PathTrie* prefix) {
  std::vector<std::string> ngram;
  PathTrie* current_node = prefix;
  PathTrie* new_node = nullptr;

  for (int order = 0; order < _max_order; order++) {
    std::vector<int> prefix_vec;

    if (_is_character_based) {
      new_node = current_node->get_path_vec(prefix_vec, _SPACE_ID, 1);
      current_node = new_node;
    } else {
      new_node = current_node->get_path_vec(prefix_vec, _SPACE_ID);
      current_node = new_node->parent;  // Skipping spaces
    }

    // reconstruct word
    std::string word = vec2str(prefix_vec);
    ngram.push_back(word);

    if (new_node->character == -1) {
      // No more spaces, but still need order
      for (int i = 0; i < _max_order - order - 1; i++) {
        ngram.push_back(START_TOKEN);
      }
      break;
    }
  }
  std::reverse(ngram.begin(), ngram.end());
  return ngram;
}

void Scorer::fill_dictionary(bool add_space) {
  fst::StdVectorFst dictionary;
  // First reverse char_list so ints can be accessed by chars
  std::unordered_map<std::string, int> char_map;
  for (unsigned int i = 0; i < _char_list.size(); i++) {
    char_map[_char_list[i]] = i;
  }

  // For each unigram convert to ints and put in trie
  int vocab_size = 0;
  for (const auto& word : _vocabulary) {
    bool added = add_word_to_dictionary(
        word, char_map, add_space, _SPACE_ID, &dictionary);
    vocab_size += added ? 1 : 0;
  }

  std::cerr << "Vocab Size " << vocab_size << std::endl;

  /* Simplify FST

   * This gets rid of "epsilon" transitions in the FST.
   * These are transitions that don't require a string input to be taken.
   * Getting rid of them is necessary to make the FST determinisitc, but
   * can greatly increase the size of the FST
   */
  fst::RmEpsilon(&dictionary);
  fst::StdVectorFst* new_dict = new fst::StdVectorFst;

  /* This makes the FST deterministic, meaning for any string input there's
   * only one possible state the FST could be in.  It is assumed our
   * dictionary is deterministic when using it.
   * (lest we'd have to check for multiple transitions at each state)
   */
  fst::Determinize(dictionary, new_dict);

  /* Finds the simplest equivalent fst. This is unnecessary but decreases
   * memory usage of the dictionary
   */
  fst::Minimize(new_dict);
  this->dictionary = new_dict;
}
