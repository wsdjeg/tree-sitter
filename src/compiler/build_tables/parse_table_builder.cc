#include "compiler/build_tables/parse_table_builder.h"
#include <algorithm>
#include <map>
#include <set>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include "compiler/parse_table.h"
#include "compiler/build_tables/parse_item.h"
#include "compiler/build_tables/parse_item_set_builder.h"
#include "compiler/lexical_grammar.h"
#include "compiler/syntax_grammar.h"
#include "compiler/rule.h"
#include "compiler/build_tables/lex_table_builder.h"

namespace tree_sitter {
namespace build_tables {

using std::deque;
using std::find;
using std::vector;
using std::set;
using std::tuple;
using std::make_tuple;
using std::map;
using std::move;
using std::string;
using std::to_string;
using std::unique_ptr;
using std::unordered_map;
using rules::Associativity;
using rules::Symbol;
using rules::END_OF_INPUT;

using SymbolSequence = vector<Symbol>;

struct ParseStateQueueEntry {
  SymbolSequence preceding_symbols;
  ParseItemSet item_set;
  ParseStateId state_id;
};

class ParseTableBuilderImpl : public ParseTableBuilder {
  const SyntaxGrammar grammar;
  const LexicalGrammar lexical_grammar;
  unordered_map<ParseItemSet, ParseStateId> state_ids_by_item_set;
  vector<const ParseItemSet *> item_sets_by_state_id;
  deque<ParseStateQueueEntry> parse_state_queue;
  ParseTable parse_table;
  ParseItemSetBuilder item_set_builder;
  unique_ptr<LexTableBuilder> lex_table_builder;
  set<ParseAction> fragile_reductions;
  vector<LookaheadSet> following_tokens_by_token;
  vector<LookaheadSet> coincident_tokens_by_token;

 public:
  ParseTableBuilderImpl(const SyntaxGrammar &syntax_grammar, const LexicalGrammar &lexical_grammar)
    : grammar(syntax_grammar),
      lexical_grammar(lexical_grammar),
      item_set_builder(syntax_grammar, lexical_grammar),
      following_tokens_by_token(lexical_grammar.variables.size()),
      coincident_tokens_by_token(lexical_grammar.variables.size()) {

    for (unsigned i = 0, n = lexical_grammar.variables.size(); i < n; i++) {
      coincident_tokens_by_token[i].insert(rules::END_OF_INPUT());
      if (lexical_grammar.variables[i].is_string) {
        for (unsigned j = 0; j < i; j++) {
          if (lexical_grammar.variables[j].is_string) {
            coincident_tokens_by_token[i].insert(Symbol::terminal(j));
            coincident_tokens_by_token[j].insert(Symbol::terminal(i));
          }
        }
      }
    }
  }

  tuple<ParseTable, LexTable, CompileError> build() {
    // Ensure that the empty rename sequence has index 0.
    parse_table.alias_sequences.push_back({});

    // Ensure that the error state has index 0.
    ParseStateId error_state_id = add_parse_state({}, ParseItemSet{});

    // Add the starting state.
    Symbol start_symbol = Symbol::non_terminal(0);
    Production start_production({{start_symbol, 0, rules::AssociativityNone, rules::Alias{}}}, 0);

    add_parse_state({}, ParseItemSet{{
      {
        ParseItem(rules::START(), start_production, 0),
        LookaheadSet({END_OF_INPUT()}),
      },
    }});

    CompileError error = process_part_state_queue();
    if (error) return make_tuple(parse_table, LexTable(), error);

    lex_table_builder = LexTableBuilder::create(
      grammar,
      lexical_grammar,
      following_tokens_by_token,
      coincident_tokens_by_token
    );

    build_error_parse_state(error_state_id);
    mark_fragile_actions();
    remove_duplicate_parse_states();

    auto lex_table = lex_table_builder->build(&parse_table);
    return make_tuple(parse_table, lex_table, CompileError::none());
  }

 private:
  CompileError process_part_state_queue() {
    while (!parse_state_queue.empty()) {
      auto entry = parse_state_queue.front();
      parse_state_queue.pop_front();

      item_set_builder.apply_transitive_closure(&entry.item_set);
      string conflict = add_actions(
        move(entry.preceding_symbols),
        move(entry.item_set),
        entry.state_id
      );

      if (!conflict.empty()) {
        return CompileError(TSCompileErrorTypeParseConflict, conflict);
      }
    }

    return CompileError::none();
  }

  void build_error_parse_state(ParseStateId state_id) {
    for (unsigned i = 0; i < lexical_grammar.variables.size(); i++) {
      Symbol token = Symbol::terminal(i);
      const LexicalVariable &variable = lexical_grammar.variables[i];

      bool exclude_from_recovery_state = false;
      for (Symbol incompatible_token : lex_table_builder->get_incompatible_tokens(i)) {
        if (!coincident_tokens_by_token[i].contains(incompatible_token) &&
            ((lexical_grammar.variables[incompatible_token.index].is_string && !variable.is_string) ||
             !lex_table_builder->get_incompatible_tokens(incompatible_token.index).count(token))) {
          exclude_from_recovery_state = true;
          break;
        }
      }
      if (!exclude_from_recovery_state) {
        parse_table.add_terminal_action(state_id, Symbol::terminal(i), ParseAction::Recover());
      }
    }

    for (const Symbol &symbol : grammar.extra_tokens) {
      if (!parse_table.states[state_id].terminal_entries.count(symbol)) {
        parse_table.add_terminal_action(state_id, symbol, ParseAction::ShiftExtra());
      }
    }

    for (size_t i = 0; i < grammar.external_tokens.size(); i++) {
      if (grammar.external_tokens[i].corresponding_internal_token == rules::NONE()) {
        parse_table.states[state_id].terminal_entries[Symbol::external(i)].actions.push_back(ParseAction::Recover());
      }
    }

    parse_table.add_terminal_action(state_id, END_OF_INPUT(), ParseAction::Recover());
  }

  ParseStateId add_parse_state(SymbolSequence &&preceding_symbols, const ParseItemSet &item_set) {
    ParseStateId new_state_id = parse_table.states.size();
    auto insertion = state_ids_by_item_set.insert({move(item_set), new_state_id});
    if (insertion.second) {
      item_sets_by_state_id.push_back(&insertion.first->first);
      parse_table.states.push_back(ParseState());
      parse_state_queue.push_back({
        move(preceding_symbols),
        insertion.first->first,
        new_state_id
      });
      return new_state_id;
    } else {
      return insertion.first->second;
    }
  }

  string add_actions(SymbolSequence &&sequence, ParseItemSet &&item_set, ParseStateId state_id) {
    map<Symbol, ParseItemSet> terminal_successors;
    map<Symbol::Index, ParseItemSet> nonterminal_successors;
    set<Symbol> lookaheads_with_conflicts;

    for (const auto &pair : item_set.entries) {
      const ParseItem &item = pair.first;
      const LookaheadSet &lookahead_symbols = pair.second;

      // If the item is finished, immediately add a Reduce or Accept action to
      // the parse table for each of its lookahead terminals.
      if (item.is_done()) {
        ParseAction action = item.lhs() == rules::START() ?
          ParseAction::Accept() :
          ParseAction::Reduce(
            item.lhs(),
            item.step_index,
            item.precedence(),
            item.production->dynamic_precedence,
            item.associativity(),
            get_alias_sequence_id(*item.production)
          );

        lookahead_symbols.for_each([&](Symbol lookahead) {
          ParseTableEntry &entry = parse_table.states[state_id].terminal_entries[lookahead];

          // Only add the highest-precedence Reduce actions to the parse table.
          // If other lower-precedence actions are possible, ignore them.
          if (entry.actions.empty()) {
            parse_table.add_terminal_action(state_id, lookahead, action);
          } else {
            ParseAction &existing_action = entry.actions[0];
            if (existing_action.type == ParseActionTypeAccept) {
              entry.actions.push_back(action);
            } else {
              if (action.precedence > existing_action.precedence) {
                for (const ParseAction &old_action : entry.actions) {
                  fragile_reductions.insert(old_action);
                }
                entry.actions.assign({action});
                lookaheads_with_conflicts.erase(lookahead);
              } else if (action.precedence == existing_action.precedence) {
                entry.actions.push_back(action);
                lookaheads_with_conflicts.insert(lookahead);
              } else {
                fragile_reductions.insert(action);
              }
            }
          }
        });

      // If the item is unfinished, create a new item by advancing one symbol.
      // Add that new item to a successor item set.
      } else {
        Symbol symbol = item.production->at(item.step_index).symbol;
        ParseItem new_item(item.lhs(), *item.production, item.step_index + 1);

        if (symbol.is_non_terminal()) {
          nonterminal_successors[symbol.index].entries[new_item] = lookahead_symbols;
        } else {
          terminal_successors[symbol].entries[new_item] = lookahead_symbols;
        }
      }
    }

    // Add a Shift action for each possible successor state. Shift actions for
    // terminal lookaheads can conflict with Reduce actions added previously.
    for (auto &pair : terminal_successors) {
      Symbol lookahead = pair.first;
      ParseItemSet &next_item_set = pair.second;
      ParseStateId next_state_id = add_parse_state(append_symbol(sequence, lookahead), next_item_set);

      if (!parse_table.states[state_id].terminal_entries[lookahead].actions.empty()) {
        lookaheads_with_conflicts.insert(lookahead);
      }

      parse_table.add_terminal_action(state_id, lookahead, ParseAction::Shift(next_state_id));
    }

    // Add a Shift action for each non-terminal transition.
    for (auto &pair : nonterminal_successors) {
      Symbol lookahead = Symbol::non_terminal(pair.first);
      ParseItemSet &next_item_set = pair.second;
      ParseStateId next_state_id = add_parse_state(append_symbol(sequence, lookahead), next_item_set);
      parse_table.set_nonterminal_action(state_id, lookahead.index, next_state_id);
    }

    for (Symbol lookahead : lookaheads_with_conflicts) {
      string conflict = handle_conflict(lookahead, item_set, sequence, state_id);
      if (!conflict.empty()) return conflict;
    }

    ParseAction shift_extra = ParseAction::ShiftExtra();
    ParseState &state = parse_table.states[state_id];
    for (const Symbol &extra_symbol : grammar.extra_tokens) {
      if (!state.terminal_entries.count(extra_symbol) || state.has_shift_action()) {
        parse_table.add_terminal_action(state_id, extra_symbol, shift_extra);
      }
    }

    auto &terminals = state.terminal_entries;
    for (auto iter = terminals.begin(), end = terminals.end(); iter != end; ++iter) {
      if (iter->first.is_built_in() || iter->first.is_external()) continue;
      for (auto other_iter = terminals.begin(); other_iter != iter; ++other_iter) {
        if (other_iter->first.is_built_in() || other_iter->first.is_external()) continue;
        coincident_tokens_by_token[iter->first.index].insert(other_iter->first);
        coincident_tokens_by_token[other_iter->first.index].insert(iter->first);
      }
    }

    return "";
  }

  void mark_fragile_actions() {
    for (ParseState &state : parse_table.states) {
      for (auto &entry : state.terminal_entries) {
        auto &actions = entry.second.actions;

        for (ParseAction &action : actions) {
          if (action.type == ParseActionTypeReduce) {
            if (action_is_fragile(action)) {
              action.fragile = true;
            }
            action.precedence = 0;
            action.associativity = rules::AssociativityNone;
          }
        }

        for (auto i = actions.begin(); i != actions.end();) {
          bool erased = false;
          for (auto j = actions.begin(); j != i; j++) {
            if (*j == *i) {
              actions.erase(i);
              erased = true;
              break;
            }
          }
          if (!erased) {
            ++i;
          }
        }
      }
    }
  }

  bool action_is_fragile(const ParseAction &action) {
    for (auto &fragile_action : fragile_reductions) {
      if (fragile_action.symbol == action.symbol &&
          fragile_action.consumed_symbol_count == action.consumed_symbol_count &&
          fragile_action.dynamic_precedence == action.dynamic_precedence) {
        if (fragile_action.precedence > action.precedence) {
          return true;
        }

        if (fragile_action.precedence == action.precedence &&
            (fragile_action.associativity == action.associativity ||
             fragile_action.associativity == rules::AssociativityLeft ||
             action.associativity == rules::AssociativityRight)) {
          return true;
        }
      }
    }

    return false;
  }

  void remove_duplicate_parse_states() {
    unordered_map<size_t, set<ParseStateId>> state_indices_by_signature;

    for (auto &pair : state_ids_by_item_set) {
      const ParseItemSet &item_set = pair.first;
      ParseStateId state_id = pair.second;
      state_indices_by_signature[item_set.unfinished_item_signature()].insert(state_id);
    }

    set<ParseStateId> deleted_states;

    while (true) {
      map<ParseStateId, ParseStateId> state_replacements;

      for (auto &pair : state_indices_by_signature) {
        auto &state_indices = pair.second;

        for (auto i = state_indices.begin(), end = state_indices.end(); i != end;) {
          for (ParseStateId j : state_indices) {
            if (j == *i) {
              ++i;
              break;
            }
            if (!state_replacements.count(j) && merge_parse_state(j, *i)) {
              state_replacements.insert({*i, j});
              deleted_states.insert(*i);
              i = state_indices.erase(i);
              break;
            }
          }
        }
      }

      if (state_replacements.empty()) break;

      for (ParseStateId i = 0, n = parse_table.states.size(); i < n; i++) {
        if (!state_replacements.count(i)) {
          ParseState &state = parse_table.states[i];
          state.each_referenced_state([&state_replacements](ParseStateId *state_index) {
            auto replacement = state_replacements.find(*state_index);
            if (replacement != state_replacements.end()) {
              *state_index = replacement->second;
            }
          });
        }
      }
    }

    vector<ParseStateId> new_state_ids(parse_table.states.size());
    size_t deleted_state_count = 0;
    auto deleted_state_iter = deleted_states.begin();
    for (ParseStateId i = 0; i < new_state_ids.size(); i++) {
      while (deleted_state_iter != deleted_states.end() && *deleted_state_iter < i) {
        deleted_state_count++;
        deleted_state_iter++;
      }
      new_state_ids[i] = i - deleted_state_count;
    }

    ParseStateId original_state_index = 0;
    auto iter = parse_table.states.begin();
    while (iter != parse_table.states.end()) {
      if (deleted_states.count(original_state_index)) {
        iter = parse_table.states.erase(iter);
      } else {
        ParseState &state = *iter;
        state.each_referenced_state([&new_state_ids](ParseStateId *state_index) {
          *state_index = new_state_ids[*state_index];
        });
        ++iter;
      }
      original_state_index++;
    }
  }

  static bool has_actions(const ParseState &state, const ParseTableEntry &entry) {
    for (const auto &pair : state.terminal_entries)
      if (pair.second.actions == entry.actions)
        return true;
    return false;
  }

  bool merge_parse_state(size_t left_index, size_t right_index) {
    ParseState &left_state = parse_table.states[left_index];
    ParseState &right_state = parse_table.states[right_index];
    if (left_state.nonterminal_entries != right_state.nonterminal_entries) return false;

    set<Symbol> symbols_to_merge;
    for (auto &left_entry : left_state.terminal_entries) {
      Symbol lookahead = left_entry.first;

      const auto &right_entry = right_state.terminal_entries.find(lookahead);
      if (right_entry == right_state.terminal_entries.end()) {
        if (lookahead.is_external()) return false;
        if (left_entry.second.actions.back().type != ParseActionTypeReduce) return false;
        if (!has_actions(right_state, left_entry.second)) return false;
        if (!lookahead.is_built_in()) {
          for (const Symbol &incompatible_token : lex_table_builder->get_incompatible_tokens(lookahead.index)) {
            if (right_state.terminal_entries.count(incompatible_token)) return false;
          }
        }
      } else {
        if (right_entry->second.actions != left_entry.second.actions) return false;
      }
    }

    for (auto &right_entry : right_state.terminal_entries) {
      Symbol lookahead = right_entry.first;

      const auto &left_entry = left_state.terminal_entries.find(lookahead);
      if (left_entry == left_state.terminal_entries.end()) {
        if (lookahead.is_external()) return false;
        if (right_entry.second.actions.back().type != ParseActionTypeReduce) return false;
        if (!has_actions(left_state, right_entry.second)) return false;
        if (!lookahead.is_built_in()) {
          for (const Symbol &incompatible_token : lex_table_builder->get_incompatible_tokens(lookahead.index)) {
            if (left_state.terminal_entries.count(incompatible_token)) return false;
          }
        }

        symbols_to_merge.insert(lookahead);
      }
    }

    for (const Symbol &lookahead : symbols_to_merge) {
      left_state.terminal_entries[lookahead] = right_state.terminal_entries[lookahead];
    }

    return true;
  }

  string handle_conflict(Symbol lookahead, const ParseItemSet &item_set,
                         const SymbolSequence &preceding_symbols, ParseStateId state_id) {
    ParseTableEntry &entry = parse_table.states[state_id].terminal_entries[lookahead];
    bool considered_associativity = false;
    int reduction_precedence = entry.actions.front().precedence;

    PrecedenceRange shift_precedence;
    set<ParseItem> conflicting_items;
    for (auto &pair : item_set.entries) {
      const ParseItem &item = pair.first;
      if (item.is_done()) {
        if (pair.second.contains(lookahead)) {
          conflicting_items.insert(item);
        }
      } else if (item.step_index > 0) {
        LookaheadSet first_set = item_set_builder.get_first_set(item.next_symbol());
        if (first_set.contains(lookahead)) {
          shift_precedence.add(item.production->at(item.step_index - 1).precedence);
          conflicting_items.insert(item);
        }
      }
    }

    if (entry.actions.back().type == ParseActionTypeShift) {

      // If the shift action has higher precedence, prefer it over any of the
      // reduce actions.
      if (shift_precedence.min > reduction_precedence ||
          (shift_precedence.min == reduction_precedence &&
           shift_precedence.max > reduction_precedence)) {
        entry.actions.assign({entry.actions.back()});
        for (const ParseAction &action : entry.actions) {
          if (action.type == ParseActionTypeReduce) {
            fragile_reductions.insert(action);
          }
        }
      }

      // If the shift action has lower precedence, prefer the reduce actions.
      else if (shift_precedence.max < reduction_precedence ||
               (shift_precedence.max == reduction_precedence &&
                shift_precedence.min < reduction_precedence)) {
        entry.actions.pop_back();
        for (auto item_iter = conflicting_items.begin(); item_iter != conflicting_items.end();) {
          if (item_iter->is_done()) {
            ++item_iter;
          } else {
            item_iter = conflicting_items.erase(item_iter);
          }
        }
      }

      // If the shift action has the same precedence as the reduce actions,
      // consider the reduce actions' associativity. If they are all left
      // associative, prefer the reduce actions. If they are all right
      // associative, prefer the shift.
      else if (shift_precedence.min == reduction_precedence &&
               shift_precedence.max == reduction_precedence) {
        considered_associativity = true;
        bool has_non_associative_reductions = false;
        bool has_left_associative_reductions = false;
        bool has_right_associative_reductions = false;
        for (const ParseAction &action : entry.actions) {
          if (action.type != ParseActionTypeReduce) break;
          switch (action.associativity) {
            case rules::AssociativityLeft:
              has_left_associative_reductions = true;
              break;
            case rules::AssociativityRight:
              has_right_associative_reductions = true;
              break;
            default:
              has_non_associative_reductions = true;
              break;
          }
        }

        if (!has_non_associative_reductions) {
          if (has_right_associative_reductions && !has_left_associative_reductions) {
            for (const ParseAction &action : entry.actions) {
              if (action.type == ParseActionTypeReduce) {
                fragile_reductions.insert(action);
              }
            }
            entry.actions.assign({entry.actions.back()});
          } else if (has_left_associative_reductions && !has_right_associative_reductions) {
            entry.actions.pop_back();
          }
        }
      }
    }

    if (entry.actions.size() == 1) return "";

    for (const ParseAction &action : entry.actions) {
      if (action.type == ParseActionTypeReduce) {
        fragile_reductions.insert(action);
      }
    }

    set<Symbol> actual_conflict;
    for (const ParseItem &item : conflicting_items) {
      Symbol symbol = item.lhs();
      if (grammar.variables[symbol.index].type == VariableTypeAuxiliary) {
        ParseStateId preceding_state_id = 1;
        for (auto &preceding_symbol : preceding_symbols) {
          ParseState &preceding_state = parse_table.states[preceding_state_id];
          if (preceding_state.nonterminal_entries.count(symbol.index)) break;
          preceding_state_id = preceding_symbol.is_terminal() ?
            preceding_state.terminal_entries[preceding_symbol].actions.back().state_index :
            preceding_state.nonterminal_entries[preceding_symbol.index];
        }
        const ParseItemSet &preceding_item_set = *item_sets_by_state_id[preceding_state_id];
        for (auto &preceding_entry : preceding_item_set.entries) {
          if (preceding_entry.first.next_symbol() == symbol) {
            actual_conflict.insert(preceding_entry.first.lhs());
          }
        }
      } else {
        actual_conflict.insert(symbol);
      }
    }

    for (const auto &expected_conflict : grammar.expected_conflicts) {
      if (expected_conflict == actual_conflict) return "";
    }

    string description = "Unresolved conflict for symbol sequence:\n\n";
    for (auto &symbol : preceding_symbols) {
      description += "  " + symbol_name(symbol);
    }

    const string dot = "\xE2\x80\xA2";
    const string ellipsis = "\xE2\x80\xA6";

    description += "  " + dot + "  " + symbol_name(lookahead) + "  " + ellipsis;
    description += "\n\n";
    description += "Possible interpretations:\n\n";

    size_t interpretation_count = 1;
    for (const ParseItem &item : conflicting_items) {
      description += "  " + to_string(interpretation_count++) + ":";

      for (size_t i = 0; i < preceding_symbols.size() - item.step_index; i++) {
        description += "  " + symbol_name(preceding_symbols[i]);
      }

      description += "  (" + symbol_name(item.lhs());
      for (size_t i = 0; i < item.production->size(); i++) {
        if (i == item.step_index) {
          description += "  " + dot;
        }
        description += "  " + symbol_name(item.production->at(i).symbol);
      }
      description += ")";

      if (item.is_done()) {
        description += "  " + dot + "  " + symbol_name(lookahead) + "  " + ellipsis;
      }

      description += "\n";
    }

    description += "\nPossible resolutions:\n\n";

    size_t resolution_count = 1;
    if (actual_conflict.size() > 1) {
      if (entry.actions.back().type == ParseActionTypeShift) {
        description += "  " + to_string(resolution_count++) + ":  ";
        description += "Specify a higher precedence in";
        bool is_first = true;
        for (Symbol conflict_symbol : actual_conflict) {
          for (const ParseItem &parse_item : conflicting_items) {
            if (parse_item.lhs() == conflict_symbol && !parse_item.is_done()) {
              if (!is_first) description += " and";
              description += " `" + symbol_name(conflict_symbol) + "`";
              is_first = false;
              break;
            }
          }
        }
        description += " than in the other rules.\n";
      }

      for (const ParseAction &action : entry.actions) {
        if (action.type == ParseActionTypeReduce) {
          description += "  " + to_string(resolution_count++) + ":  ";
          description += "Specify a higher precedence in `";
          description += symbol_name(action.symbol);
          description += "` than in the other rules.\n";
        }
      }
    }

    if (considered_associativity) {
      description += "  " + to_string(resolution_count++) + ":  ";
      description += "Specify a left or right associativity in";
      bool is_first = true;
      for (const ParseAction &action : entry.actions) {
        if (action.type == ParseActionTypeReduce) {
          if (!is_first) description += " and";
          description += " `" + symbol_name(action.symbol) + "`";
          is_first = false;
        }
      }
      description += "\n";
    }

    description += "  " + to_string(resolution_count++) + ":  ";
    description += "Add a conflict for these rules:";
    for (Symbol conflict_symbol : actual_conflict) {
      description += " `" + symbol_name(conflict_symbol) + "`";
    }
    description += "\n";
    return description;
  }

  string symbol_name(const rules::Symbol &symbol) const {
    if (symbol.is_built_in()) {
      if (symbol == END_OF_INPUT())
        return "END_OF_INPUT";
      else
        return "";
    }

    switch (symbol.type) {
      case Symbol::Terminal: {
        const LexicalVariable &variable = lexical_grammar.variables[symbol.index];
        if (variable.type == VariableTypeNamed)
          return variable.name;
        else
          return "'" + variable.name + "'";
      }
      case Symbol::NonTerminal: {
        return grammar.variables[symbol.index].name;
      }
      case Symbol::External:
      default: {
        return grammar.external_tokens[symbol.index].name;
      }
    }
  }

  unsigned get_alias_sequence_id(const Production &production) {
    bool has_alias = false;
    AliasSequence alias_sequence;
    for (unsigned i = 0, n = production.size(); i < n; i++) {
      auto &step = production.at(i);
      if (!step.alias.value.empty()) {
        has_alias = true;
        alias_sequence.resize(i + 1);
        alias_sequence[i] = step.alias;
      }
    }

    if (has_alias && production.size() > parse_table.max_alias_sequence_length) {
      parse_table.max_alias_sequence_length = production.size();
    }

    auto begin = parse_table.alias_sequences.begin();
    auto end = parse_table.alias_sequences.end();
    auto iter = find(begin, end, alias_sequence);
    if (iter != end) {
      return iter - begin;
    } else {
      parse_table.alias_sequences.push_back(move(alias_sequence));
      return parse_table.alias_sequences.size() - 1;
    }
  }

  SymbolSequence append_symbol(const SymbolSequence &sequence, const Symbol &symbol) {
    if (!sequence.empty()) {
      const LookaheadSet &left_tokens = item_set_builder.get_last_set(sequence.back());
      const LookaheadSet &right_tokens = item_set_builder.get_first_set(symbol);

      if (!left_tokens.empty() && !right_tokens.empty()) {
        left_tokens.for_each([&](Symbol left_symbol) {
          if (left_symbol.is_terminal() && !left_symbol.is_built_in()) {
            right_tokens.for_each([&](Symbol right_symbol) {
              if (right_symbol.is_terminal() && !right_symbol.is_built_in()) {
                following_tokens_by_token[left_symbol.index].insert(right_symbol);
              }
            });
          }
        });
      }
    }

    SymbolSequence result(sequence.size() + 1);
    result.assign(sequence.begin(), sequence.end());
    result.push_back(symbol);
    return result;
  }
};

unique_ptr<ParseTableBuilder> ParseTableBuilder::create(
  const SyntaxGrammar &syntax_grammar,
  const LexicalGrammar &lexical_grammar
) {
  return unique_ptr<ParseTableBuilder>(new ParseTableBuilderImpl(syntax_grammar, lexical_grammar));
}

tuple<ParseTable, LexTable, CompileError> ParseTableBuilder::build() {
  return static_cast<ParseTableBuilderImpl *>(this)->build();
}

}  // namespace build_tables
}  // namespace tree_sitter
