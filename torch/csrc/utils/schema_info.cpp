#include <ATen/core/dispatch/Dispatcher.h>
#include <torch/csrc/utils/schema_info.h>

namespace torch {
namespace utils {
void SchemaInfo::addArgumentValue(
    const std::string& name,
    const at::IValue& value) {
  c10::optional<int> index = schema_.argumentIndexWithName(name);
  TORCH_INTERNAL_ASSERT(
      index != c10::nullopt, "Schema has no argument named ", name);
  value_map_[name] = value;
  alias_maps_current_ = false;
}

void SchemaInfo::addArgumentValues(
    const std::vector<c10::optional<at::IValue>>& value_list) {
  TORCH_INTERNAL_ASSERT(
      value_list.size() <= schema_.arguments().size(),
      "Schema does not have enough arguments for value list");

  for (size_t i = 0; i < value_list.size(); i++) {
    if (value_list[i] != c10::nullopt) {
      value_map_[schema_.arguments()[i].name()] = *(value_list[i]);
      alias_maps_current_ = false;
    }
  }
}

void SchemaInfo::addArgumentValues(
    const std::unordered_map<std::string, at::IValue>& values) {
  for (const auto& key_pair : values) {
    addArgumentValue(key_pair.first, key_pair.second);
  }
}

bool SchemaInfo::hasInputArgumentNamed(const std::string& name) const {
  return std::any_of(
      schema_.arguments().begin(),
      schema_.arguments().end(),
      [&name](const c10::Argument& arg) { return arg.name() == name; });
}

bool SchemaInfo::is_mutable() {
  for (size_t i = 0; i < schema_.arguments().size(); i++) {
    if (is_mutable({c10::SchemaArgType::input, i})) {
      return true;
    }
  }
  return false;
}

bool SchemaInfo::is_mutable(const c10::SchemaArgument& argument) {
  TORCH_INTERNAL_ASSERT(
      argument.index < schema_.getCorrectList(argument.type).size(),
      "Invalid index for schema.");
  if (!alias_maps_current_) {
    generateAliasMaps();
  }
  static const std::vector<c10::FunctionSchema> training_ops = getTrainingOps();
  const auto& correct_map = (argument.type == c10::SchemaArgType::input)
      ? input_alias_map_
      : output_alias_map_;
  // Note that the training_op checks depend on index because
  // of cases where either running_mean or running_var alias another input
  // argument causing its alias status to change.
  return std::any_of(
      correct_map[argument.index].begin(),
      correct_map[argument.index].end(),
      [this](size_t aliasing_index) {
        bool special_case =
            (this->schema_.arguments()[aliasing_index].name() ==
                 "running_mean" ||
             this->schema_.arguments()[aliasing_index].name() == "running_var");
        bool is_training_op = std::any_of(
            training_ops.begin(),
            training_ops.end(),
            [this](const c10::FunctionSchema& training_op) {
              return this->schema_ == training_op;
            });
        if (special_case && is_training_op) {
          bool has_training = (hasInputArgumentNamed("training") &&
                               !value_map_.count("training")) ||
              (value_map_.count("training") &&
               value_map_.at("training").toBool());
          bool has_train =
              (hasInputArgumentNamed("train") && !value_map_.count("train")) ||
              (value_map_.count("train") && value_map_.at("train").toBool());
          bool has_use_input_stats =
              (hasInputArgumentNamed("use_input_stats") &&
               !value_map_.count("use_input_stats")) ||
              (value_map_.count("use_input_stats") &&
               value_map_.at("use_input_stats").toBool());
          return has_training || has_train || has_use_input_stats;
        } else {
          return this->schema_.is_mutable(
              {c10::SchemaArgType::input, aliasing_index});
        }
      });
}

bool SchemaInfo::is_mutable(c10::string_view name) {
  c10::optional<int> index = schema_.argumentIndexWithName(name);
  TORCH_INTERNAL_ASSERT(
      index != c10::nullopt, "Schema has no argument named ", name);

  return is_mutable({c10::SchemaArgType::input, static_cast<size_t>(*index)});
}

bool SchemaInfo::is_nondeterministic() const {
  static const c10::FunctionSchema dropout_schema = torch::jit::parseSchema(
      "aten::dropout(Tensor input, float p, bool train) -> Tensor");
  if (dropout_schema == schema_ && value_map_.count("train") &&
      !value_map_.at("train").toBool()) {
    return false;
  }

  const auto& op = c10::Dispatcher::singleton().findOp(
      c10::OperatorName(schema_.name(), schema_.overload_name()));

  return op && op->hasTag(at::Tag::nondeterministic_seeded);
}

bool SchemaInfo::may_alias(
    const c10::SchemaArgument& lhs,
    const c10::SchemaArgument& rhs) {
  bool basic_check = schema_.may_alias(lhs, rhs);
  if (basic_check) {
    return true;
  }
  c10::optional<c10::AliasTypeSet> lhsAliasTypeSet =
      schema_.mapTypeToAliasTypeSet(
          schema_.getCorrectList(lhs.type)[lhs.index].type());
  c10::optional<c10::AliasTypeSet> rhsAliasTypeSet =
      schema_.mapTypeToAliasTypeSet(
          schema_.getCorrectList(rhs.type)[rhs.index].type());
  bool types_can_alias =
      schema_.canAliasTypeSetsAlias(lhsAliasTypeSet, rhsAliasTypeSet);
  if (!types_can_alias) {
    return false;
  }

  if (!alias_maps_current_) {
    generateAliasMaps();
  }
  bool wildcard_alias_check =
      wildcard_set_.count(lhs) && wildcard_set_.count(rhs);
  if (wildcard_alias_check) {
    return true;
  }

  if (lhs.type == c10::SchemaArgType::input &&
      rhs.type == c10::SchemaArgType::input) {
    return input_alias_map_[lhs.index].count(rhs.index);
  } else if (
      lhs.type == c10::SchemaArgType::output &&
      rhs.type == c10::SchemaArgType::output) {
    for (size_t lhs_alias_input : output_alias_map_[lhs.index]) {
      if (output_alias_map_[rhs.index].count(lhs_alias_input)) {
        return true;
      }
    }
    return false;
  } else if (lhs.type == c10::SchemaArgType::output) {
    return output_alias_map_[lhs.index].count(rhs.index);
  } else {
    return output_alias_map_[rhs.index].count(lhs.index);
  }
}

bool SchemaInfo::may_contain_alias(
    const c10::SchemaArgument& lhs,
    const c10::SchemaArgument& rhs,
    bool bidirectional) {
  bool basic_check = schema_.may_contain_alias(lhs, rhs) || may_alias(lhs, rhs);
  if (basic_check) {
    return true;
  }
  if (!alias_maps_current_) {
    generateAliasMaps();
  }
  if (bidirectional) {
    return mayContainAliasImpl(lhs, rhs) || mayContainAliasImpl(rhs, lhs);
  } else {
    return mayContainAliasImpl(lhs, rhs);
  }
}

bool SchemaInfo::mayContainAliasImpl(
    const c10::SchemaArgument& lhs,
    const c10::SchemaArgument& rhs) {
  c10::optional<c10::AliasTypeSet> lhsContainedAliasTypeSet =
      schema_.getAliasTypeSetContainedTypes(schema_.mapTypeToAliasTypeSet(
          schema_.getCorrectList(lhs.type)[lhs.index].type()));
  c10::optional<c10::AliasTypeSet> rhsAliasTypeSet =
      schema_.mapTypeToAliasTypeSet(
          schema_.getCorrectList(rhs.type)[rhs.index].type());
  bool types_can_alias =
      schema_.canAliasTypeSetsAlias(lhsContainedAliasTypeSet, rhsAliasTypeSet);
  return types_can_alias && container_set_.count(lhs) &&
      wildcard_set_.count(rhs);
}

void SchemaInfo::ensureConservativity(
    const std::unordered_set<at::Symbol>& duplicates,
    const std::vector<c10::Argument>& arguments_list,
    c10::SchemaArgType type) {
  for (size_t i = 0; i < arguments_list.size(); i++) {
    if (arguments_list[i].alias_info()) {
      for (const auto& set : arguments_list[i].alias_info()->afterSets()) {
        if (duplicates.count(set)) {
          wildcard_set_.insert({type, i});
        }
      }
    }
  }
}

std::vector<c10::FunctionSchema> SchemaInfo::getTrainingOps() {
  // This is a list of ops where the a boolean variable (either "training",
  // "train" or "use_input_stats") affects the mutability of running_mean and
  // running_var
  static const std::vector<std::string> training_op_strings = {
      "aten::batch_norm(Tensor input, Tensor? weight, Tensor? bias, Tensor? running_mean, Tensor? running_var, bool training, float momentum, float eps, bool cudnn_enabled) -> Tensor",
      "aten::instance_norm(Tensor input, Tensor? weight, Tensor? bias, Tensor? running_mean, Tensor? running_var, bool use_input_stats, float momentum, float eps, bool cudnn_enabled) -> Tensor",
      "aten::_batch_norm_impl_index(Tensor input, Tensor? weight, Tensor? bias, Tensor? running_mean, Tensor? running_var, bool training, float momentum, float eps, bool cudnn_enabled) -> (Tensor, Tensor, Tensor, Tensor, int)",
      "aten::cudnn_batch_norm(Tensor input, Tensor weight, Tensor? bias, Tensor? running_mean, Tensor? running_var, bool training, float exponential_average_factor, float epsilon) -> (Tensor, Tensor, Tensor, Tensor)",
      "aten::miopen_batch_norm(Tensor input, Tensor weight, Tensor? bias, Tensor? running_mean, Tensor? running_var, bool training, float exponential_average_factor, float epsilon) -> (Tensor, Tensor, Tensor)",
      "aten::native_batch_norm(Tensor input, Tensor? weight, Tensor? bias, Tensor? running_mean, Tensor? running_var, bool training, float momentum, float eps) -> (Tensor, Tensor, Tensor)",
      "aten::native_batch_norm.out(Tensor input, Tensor? weight, Tensor? bias, Tensor? running_mean, Tensor? running_var, bool training, float momentum, float eps, *, Tensor(a!) out, Tensor(b!) save_mean, Tensor(c!) save_invstd) -> (Tensor(a!), Tensor(b!), Tensor(c!))",
  };

  std::vector<c10::FunctionSchema> training_ops;
  training_ops.reserve(training_op_strings.size());
  for (const std::string& signature : training_op_strings) {
    training_ops.push_back(torch::jit::parseSchema(signature));
  }

  return training_ops;
}

void SchemaInfo::initSchemaInfo() {
  std::unordered_set<at::Symbol> duplicates;
  auto init_schema_arguments = [this, &duplicates](
                                   const std::vector<c10::Argument>&
                                       arguments_list,
                                   c10::SchemaArgType type) {
    std::unordered_set<at::Symbol> seen;
    for (size_t i = 0; i < arguments_list.size(); i++) {
      const c10::Argument& argument = arguments_list[i];
      if (argument.alias_info()) {
        if (argument.alias_info()->isWildcardAfter()) {
          wildcard_set_.insert({type, i});
        } else {
          // This check is to ensure that the FunctionSchema will accurately
          // be represented when calling may_alias and may_contain_alias
          // on schemas with more than one argument within arguments_list that
          // shares an alias set.
          for (const auto& set : argument.alias_info()->afterSets()) {
            if (seen.count(set)) {
              TORCH_WARN(
                  set.toQualString(),
                  " appears twice in same argument list which will make aliasing checks more conservative.");
              duplicates.insert(set);
            } else {
              seen.insert(set);
            }
          }
        }
      }
      c10::optional<c10::AliasTypeSet> contained_types =
          schema_.getAliasTypeSetContainedTypes(
              schema_.mapTypeToAliasTypeSet(argument.type()));
      if (contained_types && contained_types->size() > 0) {
        container_set_.insert({type, i});
      }
    }
  };

  init_schema_arguments(schema_.arguments(), c10::SchemaArgType::input);
  init_schema_arguments(schema_.returns(), c10::SchemaArgType::output);
  ensureConservativity(
      duplicates, schema_.arguments(), c10::SchemaArgType::input);
  ensureConservativity(
      duplicates, schema_.returns(), c10::SchemaArgType::output);
}

void SchemaInfo::generateAliasMaps() {
  alias_maps_current_ = true;
  input_alias_map_ = std::vector<std::unordered_set<size_t>>(
      schema_.arguments().size(), std::unordered_set<size_t>());
  output_alias_map_ = std::vector<std::unordered_set<size_t>>(
      schema_.returns().size(), std::unordered_set<size_t>());

  // Fills input_alias_map_
  for (size_t i = 0; i < schema_.arguments().size(); i++) {
    for (size_t j = i; j < schema_.arguments().size(); j++) {
      if (i == j) {
        input_alias_map_[i].insert(i);
      } else if (
          value_map_.count(schema_.arguments()[i].name()) &&
          value_map_.count(schema_.arguments()[j].name())) {
        if (value_map_[schema_.arguments()[i].name()].isAliasOf(
                value_map_[schema_.arguments()[j].name()])) {
          input_alias_map_[i].insert(j);
          input_alias_map_[j].insert(i);
          if (wildcard_set_.count({c10::SchemaArgType::input, i})) {
            wildcard_set_.insert({c10::SchemaArgType::input, j});
          } else if (wildcard_set_.count({c10::SchemaArgType::input, j})) {
            wildcard_set_.insert({c10::SchemaArgType::input, i});
          }
        }
      }
    }
  }

  // Fills wildcard_set with container created wildcards.
  // For instance, given the schema:
  // test(Tensor a, Tensor(*) b, Tensor[] c) -> Tensor
  // where value(a) is contained in value(c), then a will be added to the
  // wildcard set where it can now alias b.
  for (size_t i = 0; i < schema_.arguments().size(); i++) {
    for (size_t j = 0; j < schema_.arguments().size(); j++) {
      // if they are already aliasing, there is no way one contains the other
      if (!input_alias_map_[i].count(j) &&
          value_map_.count(schema_.arguments()[i].name()) &&
          value_map_.count(schema_.arguments()[j].name())) {
        c10::IValue::HashAliasedIValues subValues;
        value_map_[schema_.arguments()[i].name()].getSubValues(subValues);
        if (subValues.count(value_map_[schema_.arguments()[j].name()])) {
          wildcard_set_.insert({c10::SchemaArgType::input, j});
        }
      }
    }
  }

  // Fills output_alias_map_
  for (size_t i = 0; i < schema_.arguments().size(); i++) {
    for (size_t j = 0; j < schema_.returns().size(); j++) {
      if (schema_.may_alias(
              {c10::SchemaArgType::input, i},
              {c10::SchemaArgType::output, j})) {
        if (wildcard_set_.count({c10::SchemaArgType::input, i})) {
          wildcard_set_.insert({c10::SchemaArgType::output, j});
        }
        output_alias_map_[j].insert(
            input_alias_map_[i].begin(), input_alias_map_[i].end());
      }
    }
  }
}

} // namespace utils
} // namespace torch
