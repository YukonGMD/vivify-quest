#include "VivifyRuntimeInternal.hpp"
#include "VivifyComponents.hpp"

namespace Vivify {

std::optional<MaterialPropertyChange> Runtime::ParseMaterialProperty(rapidjson::Value const& propertyJson) {
  auto id = ReadStringView(propertyJson, "id");
  auto type = ReadStringView(propertyJson, "type");
  if (!id.has_value() || !type.has_value()) {
    return std::nullopt;
  }
  MaterialPropertyChange property;
  property.kind = ParseMaterialPropertyKind(*type);
  if (property.kind == MaterialPropertyKind::Keyword) {
    property.id = std::string(*id);
  } else {
    property.id = UnityEngine::Shader::PropertyToID(StringW(*id));
  }
  switch (property.kind) {
    case MaterialPropertyKind::Texture: {
      auto texture = ReadStringView(propertyJson, "value");
      if (!texture.has_value()) {
        return std::nullopt;
      }
      property.value = std::string(*texture);
      return property;
    }
    case MaterialPropertyKind::Color:
    case MaterialPropertyKind::Vector: {
      auto point = MakePointDefinition(propertyJson, "value", Tracks::ffi::WrapBaseValueType::Vec4);
      if (!point.has_value()) {
        return std::nullopt;
      }
      property.value = *point;
      return property;
    }
    case MaterialPropertyKind::Float: {
      if (auto point = MakePointDefinition(propertyJson, "value", Tracks::ffi::WrapBaseValueType::Float); point.has_value()) {
        property.value = *point;
      } else if (auto value = ReadFloat(propertyJson, "value"); value.has_value()) {
        property.value = *value;
      } else {
        return std::nullopt;
      }
      return property;
    }
    case MaterialPropertyKind::Int: {
      if (auto value = ReadInt(propertyJson, "value"); value.has_value()) {
        property.value = *value;
        return property;
      }
      return std::nullopt;
    }
    case MaterialPropertyKind::Keyword: {
      if (auto point = MakePointDefinition(propertyJson, "value", Tracks::ffi::WrapBaseValueType::Float); point.has_value()) {
        property.value = *point;
      } else if (auto value = ReadBool(propertyJson, "value"); value.has_value()) {
        property.value = *value;
      } else {
        return std::nullopt;
      }
      return property;
    }
    case MaterialPropertyKind::Unsupported:
    default:
      return std::nullopt;
  }
}

namespace {
// An animated material keyword is set every frame of its animation, which used
// to make a new managed string every time. One string per keyword name,
// allocated outside the collector (beatsaber-hook's Manual creation), is made
// once and reused; the keyword is still set every frame exactly as before.
StringW KeywordName(std::string const& keyword) {
  static std::unordered_map<std::string, Il2CppString*> names;
  auto it = names.find(keyword);
  if (it == names.end()) {
    it = names.emplace(keyword, il2cpp_utils::newcsstr<il2cpp_utils::CreationType::Manual>(keyword)).first;
  }
  return StringW(it->second);
}

// A Color or Vector written as one flat list of numbers is Heck's single-point
// shorthand: Heck appends a time of 0 and needs exactly four numbers before it.
// Any other count (an [r,g,b] colour) fails Heck's assert, and PC Vivify skips
// the whole event. tracks-rs instead reads [r,g,b] as (r,g,b,0), a colour with
// zero alpha, so the check has to happen here.
bool IsMalformedVectorShorthand(rapidjson::Value const& propertyJson) {
  auto type = ReadStringView(propertyJson, "type");
  if (!type.has_value()) return false;
  auto const kind = ParseMaterialPropertyKind(*type);
  if (kind != MaterialPropertyKind::Color && kind != MaterialPropertyKind::Vector) return false;
  auto* value = ReadValuePtr(propertyJson, "value");
  if (value == nullptr || !value->IsArray() || value->Empty()) return false;
  for (auto const& entry : value->GetArray()) {
    if (!entry.IsNumber()) return false;
  }
  return value->Size() != 4;
}
}

std::vector<MaterialPropertyChange> Runtime::ParseMaterialProperties(rapidjson::Value const& json, bool* malformed) {
  std::vector<MaterialPropertyChange> properties;
  if (malformed != nullptr) *malformed = false;
  auto* propertiesValue = ReadValuePtr(json, "properties");
  if (propertiesValue == nullptr || !propertiesValue->IsArray()) {
    return properties;
  }
  properties.reserve(propertiesValue->Size());
  for (auto const& propertyJson : propertiesValue->GetArray()) {
    if (!propertyJson.IsObject()) {
      continue;
    }
    if (IsMalformedVectorShorthand(propertyJson)) {
      PaperLogger.warn("Vivify: property '{}' is a {} written as {} numbers; it needs four ([r, g, b, a]). "
                       "PC Vivify skips this whole event, and so does Vivify Quest",
                       ReadStringView(propertyJson, "id").value_or("<no id>"),
                       ReadStringView(propertyJson, "type").value_or("?"),
                       ReadValuePtr(propertyJson, "value")->Size());
      if (malformed != nullptr) *malformed = true;
      return {};
    }
    if (auto property = ParseMaterialProperty(propertyJson); property.has_value()) {
      properties.emplace_back(std::move(*property));
    }
  }
  return properties;
}

std::optional<AnimatorPropertyChange> Runtime::ParseAnimatorProperty(rapidjson::Value const& propertyJson) {
  auto name = ReadStringView(propertyJson, "id");
  auto type = ReadStringView(propertyJson, "type");
  if (!name.has_value() || !type.has_value()) {
    return std::nullopt;
  }
  AnimatorPropertyChange property;
  property.name = std::string(*name);
  property.id = UnityEngine::Animator::StringToHash(StringW(*name));
  property.kind = ParseAnimatorPropertyKind(*type);
  switch (property.kind) {
    case AnimatorPropertyKind::Bool: {
      if (auto point = MakePointDefinition(propertyJson, "value", Tracks::ffi::WrapBaseValueType::Float); point.has_value()) {
        property.value = *point;
      } else if (auto value = ReadBool(propertyJson, "value"); value.has_value()) {
        property.value = *value;
      } else {
        return std::nullopt;
      }
      return property;
    }
    case AnimatorPropertyKind::Float: {
      if (auto point = MakePointDefinition(propertyJson, "value", Tracks::ffi::WrapBaseValueType::Float); point.has_value()) {
        property.value = *point;
      } else if (auto value = ReadFloat(propertyJson, "value"); value.has_value()) {
        property.value = *value;
      } else {
        return std::nullopt;
      }
      return property;
    }
    case AnimatorPropertyKind::Integer: {
      if (auto point = MakePointDefinition(propertyJson, "value", Tracks::ffi::WrapBaseValueType::Float); point.has_value()) {
        property.value = *point;
      } else if (auto value = ReadInt(propertyJson, "value"); value.has_value()) {
        property.value = *value;
      } else {
        return std::nullopt;
      }
      return property;
    }
    case AnimatorPropertyKind::Trigger: {
      property.value = ReadBool(propertyJson, "value").value_or(true);
      return property;
    }
    case AnimatorPropertyKind::Unsupported:
    default:
      return std::nullopt;
  }
}

std::vector<AnimatorPropertyChange> Runtime::ParseAnimatorProperties(rapidjson::Value const& json) {
  std::vector<AnimatorPropertyChange> properties;
  auto* propertiesValue = ReadValuePtr(json, "properties");
  if (propertiesValue == nullptr || !propertiesValue->IsArray()) {
    return properties;
  }
  properties.reserve(propertiesValue->Size());
  for (auto const& propertyJson : propertiesValue->GetArray()) {
    if (!propertyJson.IsObject()) {
      continue;
    }
    if (auto property = ParseAnimatorProperty(propertyJson); property.has_value()) {
      properties.emplace_back(std::move(*property));
    }
  }
  return properties;
}

bool Runtime::IsAnimated(MaterialPropertyChange const& property) const {
  return std::holds_alternative<PointDefinitionW>(property.value);
}

bool Runtime::IsAnimated(AnimatorPropertyChange const& property) const {
  return std::holds_alternative<PointDefinitionW>(property.value);
}

void Runtime::ApplyMaterialProperty(UnityEngine::Material* material, MaterialPropertyChange const& property, float progress) {
  if (material == nullptr) {
    return;
  }
  switch (property.kind) {
    case MaterialPropertyKind::Texture: {
      auto propertyId = std::get<int>(property.id);
      auto* texture = GetAssetAs<UnityEngine::Texture>(std::get<std::string>(property.value));
      if (texture != nullptr) {
        material->SetTexture(propertyId, texture);
      }
      break;
    }
    case MaterialPropertyKind::Color: {
      auto propertyId = std::get<int>(property.id);
      auto const& point = std::get<PointDefinitionW>(property.value);
      material->SetColor(propertyId, VectorToColor(point.InterpolateVector4(progress)));
      break;
    }
    case MaterialPropertyKind::Float: {
      auto propertyId = std::get<int>(property.id);
      if (std::holds_alternative<PointDefinitionW>(property.value)) {
        material->SetFloat(propertyId, std::get<PointDefinitionW>(property.value).InterpolateLinear(progress));
      } else {
        material->SetFloat(propertyId, std::get<float>(property.value));
      }
      break;
    }
    case MaterialPropertyKind::Int: {
      auto propertyId = std::get<int>(property.id);
      material->SetInt(propertyId, std::get<int>(property.value));
      break;
    }
    case MaterialPropertyKind::Vector: {
      auto propertyId = std::get<int>(property.id);
      auto const& point = std::get<PointDefinitionW>(property.value);
      material->SetVector(propertyId, ToUnityVector(point.InterpolateVector4(progress)));
      break;
    }
    case MaterialPropertyKind::Keyword: {
      auto const& keyword = std::get<std::string>(property.id);
      bool enabled = false;
      if (std::holds_alternative<PointDefinitionW>(property.value)) {
        enabled = std::get<PointDefinitionW>(property.value).InterpolateLinear(progress) >= 1.0f;
      } else {
        enabled = std::get<bool>(property.value);
      }
      SetMaterialKeyword(material, KeywordName(keyword), enabled);
      ApplyStereoKeywords(material);
      break;
    }
    case MaterialPropertyKind::Unsupported:
    default:
      break;
  }
}

void Runtime::SaveOriginalGlobalProperty(MaterialPropertyChange const& property) {
  switch (property.kind) {
    case MaterialPropertyKind::Texture: {
      int propertyId = std::get<int>(property.id);
      if (!_savedGlobalProperties.contains(propertyId)) {
        _savedGlobalProperties.emplace(propertyId, UnityEngine::Shader::GetGlobalTexture(propertyId).ptr());
      }
      break;
    }
    case MaterialPropertyKind::Color: {
      int propertyId = std::get<int>(property.id);
      if (!_savedGlobalProperties.contains(propertyId)) {
        _savedGlobalProperties.emplace(propertyId, UnityEngine::Shader::GetGlobalColor(propertyId));
      }
      break;
    }
    case MaterialPropertyKind::Float: {
      int propertyId = std::get<int>(property.id);
      if (!_savedGlobalProperties.contains(propertyId)) {
        _savedGlobalProperties.emplace(propertyId, UnityEngine::Shader::GetGlobalFloat(propertyId));
      }
      break;
    }
    case MaterialPropertyKind::Vector: {
      int propertyId = std::get<int>(property.id);
      if (!_savedGlobalProperties.contains(propertyId)) {
        _savedGlobalProperties.emplace(propertyId, UnityEngine::Shader::GetGlobalVector(propertyId));
      }
      break;
    }
    case MaterialPropertyKind::Keyword: {
      auto const& keyword = std::get<std::string>(property.id);
      if (!_savedGlobalKeywords.contains(keyword)) {
        _savedGlobalKeywords.emplace(keyword, UnityEngine::Shader::IsKeywordEnabled(StringW(keyword)));
      }
      break;
    }
    case MaterialPropertyKind::Int:
    case MaterialPropertyKind::Unsupported:
    default:
      break;
  }
}

void Runtime::ApplyGlobalProperty(MaterialPropertyChange const& property, float progress) {
  SaveOriginalGlobalProperty(property);
  switch (property.kind) {
    case MaterialPropertyKind::Texture: {
      int propertyId = std::get<int>(property.id);
      auto* texture = GetAssetAs<UnityEngine::Texture>(std::get<std::string>(property.value));
      if (texture != nullptr) {
        UnityEngine::Shader::SetGlobalTexture(propertyId, texture);
        _currentGlobalProperties[propertyId] = texture;
      }
      break;
    }
    case MaterialPropertyKind::Color: {
      int propertyId = std::get<int>(property.id);
      auto const& point = std::get<PointDefinitionW>(property.value);
      auto color = VectorToColor(point.InterpolateVector4(progress));
      UnityEngine::Shader::SetGlobalColor(propertyId, color);
      _currentGlobalProperties[propertyId] = color;
      break;
    }
    case MaterialPropertyKind::Float: {
      int propertyId = std::get<int>(property.id);
      float value;
      if (std::holds_alternative<PointDefinitionW>(property.value)) {
        value = std::get<PointDefinitionW>(property.value).InterpolateLinear(progress);
      } else {
        value = std::get<float>(property.value);
      }
      UnityEngine::Shader::SetGlobalFloat(propertyId, value);
      _currentGlobalProperties[propertyId] = value;
      break;
    }
    case MaterialPropertyKind::Vector: {
      int propertyId = std::get<int>(property.id);
      auto const& point = std::get<PointDefinitionW>(property.value);
      auto vector = ToUnityVector(point.InterpolateVector4(progress));
      UnityEngine::Shader::SetGlobalVector(propertyId, vector);
      _currentGlobalProperties[propertyId] = vector;
      break;
    }
    case MaterialPropertyKind::Keyword: {
      auto const& keyword = std::get<std::string>(property.id);
      bool enabled = false;
      if (std::holds_alternative<PointDefinitionW>(property.value)) {
        enabled = std::get<PointDefinitionW>(property.value).InterpolateLinear(progress) >= 1.0f;
      } else {
        enabled = std::get<bool>(property.value);
      }
      // An animated keyword is applied every frame of its animation; only a
      // change of state is worth a managed string and a keyword switch.
      if (auto known = _currentGlobalKeywords.find(keyword);
          known != _currentGlobalKeywords.end() && known->second == enabled) {
        break;
      }
      if (enabled) {
        UnityEngine::Shader::EnableKeyword(StringW(keyword));
      } else {
        UnityEngine::Shader::DisableKeyword(StringW(keyword));
      }
      _currentGlobalKeywords[keyword] = enabled;
      auto currentCamera = UnityEngine::Camera::get_current();
      SetMultipassShaderStateForCamera(currentCamera.unsafePtr());
      break;
    }
    case MaterialPropertyKind::Int:
    case MaterialPropertyKind::Unsupported:
    default:
      break;
  }
}

void Runtime::ApplyAnimatorProperty(std::vector<UnityEngine::Animator*> const& animators,
                                    AnimatorPropertyChange const& property,
                                    float progress) {
  for (auto* animator : animators) {
    if (!IsManagedAlive(animator)) {
      continue;
    }
    switch (property.kind) {
      case AnimatorPropertyKind::Bool: {
        bool value = false;
        if (std::holds_alternative<PointDefinitionW>(property.value)) {
          value = std::get<PointDefinitionW>(property.value).InterpolateLinear(progress) >= 1.0f;
        } else {
          value = std::get<bool>(property.value);
        }
        animator->SetBool(property.id, value);
        break;
      }
      case AnimatorPropertyKind::Float: {
        float value = 0.0f;
        if (std::holds_alternative<PointDefinitionW>(property.value)) {
          value = std::get<PointDefinitionW>(property.value).InterpolateLinear(progress);
        } else {
          value = std::get<float>(property.value);
        }
        animator->SetFloat(property.id, value);
        break;
      }
      case AnimatorPropertyKind::Integer: {
        int value = 0;
        if (std::holds_alternative<PointDefinitionW>(property.value)) {
          value = static_cast<int>(std::get<PointDefinitionW>(property.value).InterpolateLinear(progress));
        } else {
          value = std::get<int>(property.value);
        }
        animator->SetInteger(property.id, value);
        break;
      }
      case AnimatorPropertyKind::Trigger: {
        bool enabled = std::get<bool>(property.value);
        if (enabled) {
          animator->SetTrigger(property.id);
        } else {
          animator->ResetTrigger(property.id);
        }
        break;
      }
      case AnimatorPropertyKind::Unsupported:
      default:
        break;
    }
  }
}

void Runtime::QueueMaterialPropertyAnimation(UnityEngine::Material* material,
                                             std::vector<MaterialPropertyChange> const& properties,
                                             float startTime, float duration, Functions easing) {
  std::vector<MaterialPropertyChange> animatedProperties;
  animatedProperties.reserve(properties.size());
  for (auto const& property : properties) {
    if (IsAnimated(property)) {
      animatedProperties.emplace_back(property);
    }
  }
  if (animatedProperties.empty()) return;
  _materialAnimations.emplace_back(ActiveMaterialAnimation{
      .material = material,
      .properties = std::move(animatedProperties),
      .startTime = startTime,
      .duration = duration,
      .endTime = startTime + duration,
      .easing = easing,
  });
}

void Runtime::HandleSetMaterialProperty(CustomJSONData::CustomEventData* customEventData, rapidjson::Value const& json) {
  auto asset = ReadStringView(json, "asset");
  if (!asset.has_value()) {
    return;
  }
  auto* material = GetAssetAs<UnityEngine::Material>(*asset);
  if (material == nullptr) {
    return;
  }
  auto properties = ParseMaterialProperties(json);
  if (properties.empty()) {
    return;
  }
  float duration = DurationBeatsToSeconds(ReadFloat(json, "duration").value_or(0.0f));
  Functions easing = ParseEasing(ReadStringView(json, "easing").value_or("easeLinear"sv));
  float startTime = customEventData->time;
  float currentSongTime = CurrentSongTime();
  bool completed = duration <= 0.0f || startTime + duration <= currentSongTime;
  float initialProgress = completed ? 1.0f : 0.0f;
  for (auto const& property : properties) {
    ApplyMaterialProperty(material, property, initialProgress);
  }
  if (!completed) {
    QueueMaterialPropertyAnimation(material, properties, startTime, duration, easing);
  }
}

void Runtime::HandleSetAnimatorProperty(CustomJSONData::CustomEventData* customEventData, rapidjson::Value const& json) {
  auto id = ReadStringView(json, "id");
  if (!id.has_value()) {
    return;
  }
  std::string prefabId(*id);
  auto prefabIt = _livePrefabs.find(prefabId);
  if (prefabIt == _livePrefabs.end()) {
    return;
  }
  auto properties = ParseAnimatorProperties(json);
  if (properties.empty()) {
    return;
  }
  float duration = DurationBeatsToSeconds(ReadFloat(json, "duration").value_or(0.0f));
  Functions easing = ParseEasing(ReadStringView(json, "easing").value_or("easeLinear"sv));
  float startTime = customEventData->time;
  std::vector<AnimatorPropertyChange> animatedProperties;
  animatedProperties.reserve(properties.size());
  float currentSongTime = CurrentSongTime();
  bool completed = duration <= 0.0f || startTime + duration <= currentSongTime;
  float initialProgress = completed ? 1.0f : 0.0f;
  for (auto const& property : properties) {
    ApplyAnimatorProperty(prefabIt->second.animators, property, initialProgress);
    if (!completed && IsAnimated(property)) {
      animatedProperties.emplace_back(property);
    }
  }
  if (!animatedProperties.empty()) {
    _animatorAnimations.emplace_back(ActiveAnimatorAnimation{
        .prefabId = std::move(prefabId),
        .properties = std::move(animatedProperties),
        .startTime = startTime,
        .duration = duration,
        .endTime = startTime + duration,
        .easing = easing,
    });
  }
}

void Runtime::HandleSetGlobalProperty(CustomJSONData::CustomEventData* customEventData, rapidjson::Value const& json) {
  auto properties = ParseMaterialProperties(json);
  if (properties.empty()) {
    return;
  }
  float duration = DurationBeatsToSeconds(ReadFloat(json, "duration").value_or(0.0f));
  Functions easing = ParseEasing(ReadStringView(json, "easing").value_or("easeLinear"sv));
  float startTime = customEventData->time;
  std::vector<MaterialPropertyChange> animatedProperties;
  animatedProperties.reserve(properties.size());
  float currentSongTime = CurrentSongTime();
  bool completed = duration <= 0.0f || startTime + duration <= currentSongTime;
  float initialProgress = completed ? 1.0f : 0.0f;
  for (auto const& property : properties) {
    ApplyGlobalProperty(property, initialProgress);
    if (!completed && IsAnimated(property)) {
      animatedProperties.emplace_back(property);
    }
  }
  if (!animatedProperties.empty()) {
    _globalAnimations.emplace_back(ActiveGlobalAnimation{
        .properties = std::move(animatedProperties),
        .startTime = startTime,
        .duration = duration,
        .endTime = startTime + duration,
        .easing = easing,
    });
  }
}

float Runtime::AnimationProgress(float startTime, float duration, Functions easing, float songTime) const {
  if (duration <= 0.0f) {
    return 1.0f;
  }
  float raw = std::clamp((songTime - startTime) / duration, 0.0f, 1.0f);
  return Easings::Interpolate(raw, easing);
}

void Runtime::UpdateMaterialAnimations() {
  if (_materialAnimations.empty()) return;
  float const songTime = CurrentSongTime();
  auto write = _materialAnimations.begin();
  for (auto read = _materialAnimations.begin(); read != _materialAnimations.end(); ++read) {
    if (!IsManagedAlive(read->material)) continue;
    float const progress = AnimationProgress(read->startTime, read->duration, read->easing, songTime);
    for (auto const& property : read->properties) ApplyMaterialProperty(read->material, property, progress);
    if (songTime < read->endTime) {
      if (write != read) *write = std::move(*read);
      ++write;
    }
  }
  _materialAnimations.erase(write, _materialAnimations.end());
}

void Runtime::UpdateGlobalAnimations() {
  if (_globalAnimations.empty()) return;
  float const songTime = CurrentSongTime();
  auto write = _globalAnimations.begin();
  for (auto read = _globalAnimations.begin(); read != _globalAnimations.end(); ++read) {
    float const progress = AnimationProgress(read->startTime, read->duration, read->easing, songTime);
    for (auto const& property : read->properties) ApplyGlobalProperty(property, progress);
    if (songTime < read->endTime) {
      if (write != read) *write = std::move(*read);
      ++write;
    }
  }
  _globalAnimations.erase(write, _globalAnimations.end());
}

void Runtime::UpdateAnimatorAnimations() {
  if (_animatorAnimations.empty()) return;
  float const songTime = CurrentSongTime();
  auto write = _animatorAnimations.begin();
  for (auto read = _animatorAnimations.begin(); read != _animatorAnimations.end(); ++read) {
    auto prefabIt = _livePrefabs.find(read->prefabId);
    if (prefabIt == _livePrefabs.end()) continue;
    float const progress = AnimationProgress(read->startTime, read->duration, read->easing, songTime);
    for (auto const& property : read->properties) ApplyAnimatorProperty(prefabIt->second.animators, property, progress);
    if (songTime < read->endTime) {
      if (write != read) *write = std::move(*read);
      ++write;
    }
  }
  _animatorAnimations.erase(write, _animatorAnimations.end());
}

void Runtime::RestoreGlobalProperties() {
  for (auto const& [propertyId, value] : _savedGlobalProperties) {
    if (std::holds_alternative<UnityEngine::Texture*>(value)) {
      UnityEngine::Shader::SetGlobalTexture(propertyId, std::get<UnityEngine::Texture*>(value));
    } else if (std::holds_alternative<UnityEngine::Color>(value)) {
      UnityEngine::Shader::SetGlobalColor(propertyId, std::get<UnityEngine::Color>(value));
    } else if (std::holds_alternative<float>(value)) {
      UnityEngine::Shader::SetGlobalFloat(propertyId, std::get<float>(value));
    } else if (std::holds_alternative<UnityEngine::Vector4>(value)) {
      UnityEngine::Shader::SetGlobalVector(propertyId, std::get<UnityEngine::Vector4>(value));
    }
  }
  for (auto const& [keyword, enabled] : _savedGlobalKeywords) {
    if (enabled) {
      UnityEngine::Shader::EnableKeyword(StringW(keyword));
    } else {
      UnityEngine::Shader::DisableKeyword(StringW(keyword));
    }
  }
}

void Runtime::ReapplyCurrentGlobalProperties() {
  for (auto const& [propertyId, value] : _currentGlobalProperties) {
    if (std::holds_alternative<UnityEngine::Texture*>(value)) {
      auto* texture = std::get<UnityEngine::Texture*>(value);
      if (IsManagedAlive(texture)) {
        UnityEngine::Shader::SetGlobalTexture(propertyId, texture);
      }
    } else if (std::holds_alternative<UnityEngine::Color>(value)) {
      UnityEngine::Shader::SetGlobalColor(propertyId, std::get<UnityEngine::Color>(value));
    } else if (std::holds_alternative<float>(value)) {
      UnityEngine::Shader::SetGlobalFloat(propertyId, std::get<float>(value));
    } else if (std::holds_alternative<UnityEngine::Vector4>(value)) {
      UnityEngine::Shader::SetGlobalVector(propertyId, std::get<UnityEngine::Vector4>(value));
    }
  }
  for (auto const& [keyword, enabled] : _currentGlobalKeywords) {
    if (enabled) {
      UnityEngine::Shader::EnableKeyword(StringW(keyword));
    } else {
      UnityEngine::Shader::DisableKeyword(StringW(keyword));
    }
  }
}

namespace {
enum class SettingType { Float, Color, Int, Bool, MaterialAsset, LightPrefab };

std::unordered_map<std::string_view, SettingType> const kRenderSettingTypes = {
    {"ambientEquatorColor"sv, SettingType::Color},
    {"ambientGroundColor"sv, SettingType::Color},
    {"ambientIntensity"sv, SettingType::Float},
    {"ambientLight"sv, SettingType::Color},
    {"ambientMode"sv, SettingType::Int},
    {"ambientSkyColor"sv, SettingType::Color},
    {"defaultReflectionMode"sv, SettingType::Int},
    {"defaultReflectionResolution"sv, SettingType::Int},
    {"flareFadeSpeed"sv, SettingType::Float},
    {"flareStrength"sv, SettingType::Float},
    {"fog"sv, SettingType::Bool},
    {"fogColor"sv, SettingType::Color},
    {"fogDensity"sv, SettingType::Float},
    {"fogEndDistance"sv, SettingType::Float},
    {"fogMode"sv, SettingType::Int},
    {"fogStartDistance"sv, SettingType::Float},
    {"haloStrength"sv, SettingType::Float},
    {"reflectionBounces"sv, SettingType::Int},
    {"reflectionIntensity"sv, SettingType::Float},
    {"subtractiveShadowColor"sv, SettingType::Color},
    {"skybox"sv, SettingType::MaterialAsset},
    {"sun"sv, SettingType::LightPrefab},
    {"antiAliasing"sv, SettingType::Int},
    {"realtimeReflectionProbes"sv, SettingType::Bool},

    // QualitySettings' shadow, LOD and particle knobs, plus the two XRSettings
    // ones. bs-cordl does not declare any of these, so they are reached by
    // internal-call address instead (see ResolveSettingIcall). Maps in the RSIH
    // playlist set the five shadow keys at time 0 and every one of them was
    // being dropped with an "unsupported setting" warning.
    {"shadows"sv, SettingType::Int},
    {"shadowResolution"sv, SettingType::Int},
    {"shadowProjection"sv, SettingType::Int},
    {"shadowCascades"sv, SettingType::Int},
    {"shadowmaskMode"sv, SettingType::Int},
    {"shadowDistance"sv, SettingType::Float},
    {"shadowNearPlaneOffset"sv, SettingType::Float},
    {"pixelLightCount"sv, SettingType::Int},
    {"anisotropicFiltering"sv, SettingType::Int},
    {"maximumLODLevel"sv, SettingType::Int},
    {"particleRaycastBudget"sv, SettingType::Int},
    {"skinWeights"sv, SettingType::Int},
    {"lodBias"sv, SettingType::Float},
    {"softParticles"sv, SettingType::Bool},
    {"softVegetation"sv, SettingType::Bool},
    {"billboardsFaceCameraPosition"sv, SettingType::Bool},
    {"useOcclusionMesh"sv, SettingType::Bool},
    {"occlusionMaskScale"sv, SettingType::Float},
};

// Which il2cpp class owns each setting that has to be reached by internal call.
//
// The property name on the C# side is the same as the key a map writes, so the
// symbol is just "<owner>::get_<key>" / "<owner>::set_<key>".
//
// vSyncCount is deliberately absent even though PC Vivify exposes it: on a
// standalone headset the compositor owns the frame cadence, and letting a map
// change it would do far more harm than any effect it could buy.
std::unordered_map<std::string_view, std::string_view> const kIcallSettingOwners = {
    {"realtimeReflectionProbes"sv, "UnityEngine.QualitySettings"sv},
    {"shadows"sv, "UnityEngine.QualitySettings"sv},
    {"shadowResolution"sv, "UnityEngine.QualitySettings"sv},
    {"shadowProjection"sv, "UnityEngine.QualitySettings"sv},
    {"shadowCascades"sv, "UnityEngine.QualitySettings"sv},
    {"shadowmaskMode"sv, "UnityEngine.QualitySettings"sv},
    {"shadowDistance"sv, "UnityEngine.QualitySettings"sv},
    {"shadowNearPlaneOffset"sv, "UnityEngine.QualitySettings"sv},
    {"pixelLightCount"sv, "UnityEngine.QualitySettings"sv},
    {"anisotropicFiltering"sv, "UnityEngine.QualitySettings"sv},
    {"maximumLODLevel"sv, "UnityEngine.QualitySettings"sv},
    {"particleRaycastBudget"sv, "UnityEngine.QualitySettings"sv},
    {"skinWeights"sv, "UnityEngine.QualitySettings"sv},
    {"lodBias"sv, "UnityEngine.QualitySettings"sv},
    {"softParticles"sv, "UnityEngine.QualitySettings"sv},
    {"softVegetation"sv, "UnityEngine.QualitySettings"sv},
    {"billboardsFaceCameraPosition"sv, "UnityEngine.QualitySettings"sv},
    {"useOcclusionMesh"sv, "UnityEngine.XR.XRSettings"sv},
    {"occlusionMaskScale"sv, "UnityEngine.XR.XRSettings"sv},
};

// Enum-valued settings may be written by name as well as by number -- "fogMode":
// "ExponentialSquared" rather than 3 (from gamesbeash-art's
// Vivify-Quest_enabled-Play). Names match the C# enum members, compared after
// NormalizeAssetKey, so case does not matter.
std::optional<int> ParseNamedSettingValue(std::string_view key, std::string_view value) {
  static std::unordered_map<std::string_view, std::unordered_map<std::string, int>> const kNamedValues = {
      {"fogMode"sv, {{"linear", 1}, {"exponential", 2}, {"exponentialsquared", 3}}},
      {"ambientMode"sv, {{"skybox", 0}, {"trilight", 1}, {"flat", 3}, {"custom", 4}}},
      {"defaultReflectionMode"sv, {{"skybox", 0}, {"custom", 1}}},
      {"shadows"sv, {{"disable", 0}, {"hardonly", 1}, {"all", 2}}},
      {"shadowResolution"sv, {{"low", 0}, {"medium", 1}, {"high", 2}, {"veryhigh", 3}}},
      {"shadowProjection"sv, {{"closefit", 0}, {"stablefit", 1}}},
      {"shadowmaskMode"sv, {{"shadowmask", 0}, {"distanceshadowmask", 1}}},
      {"anisotropicFiltering"sv, {{"disable", 0}, {"enable", 1}, {"forceenable", 2}}},
      {"skinWeights"sv, {{"none", 0}, {"onebone", 1}, {"twobones", 2}, {"fourbones", 4}, {"unlimited", 255}}},
  };
  auto owner = kNamedValues.find(key);
  if (owner == kNamedValues.end()) return std::nullopt;
  auto it = owner->second.find(NormalizeAssetKey(value));
  if (it == owner->second.end()) return std::nullopt;
  return it->second;
}

bool IsLiteralColorArray(rapidjson::Value const& value) {
  if (!value.IsArray() || value.Size() < 3 || value.Size() > 4) return false;
  for (auto const& entry : value.GetArray()) {
    if (!entry.IsNumber()) return false;
  }
  return true;
}

// PC calls DynamicGI.UpdateEnvironment() after every setting it sets to a
// plain value (SetRenderingSettings.cs: StructSettingHandler and
// ClassSettingHandler), so ambient light and reflections are recomputed from
// the skybox and ambient settings now in force; not after an animated value,
// nor when the settings are restored. The game strips the managed DynamicGI
// class, so this is its internal call (naomi.7).
void UpdateEnvironmentLikePc() {
  static auto* fn = reinterpret_cast<void (*)()>(ResolveEngineIcall("UnityEngine.DynamicGI::UpdateEnvironment"));
  if (fn != nullptr) fn();
}
}

void* ResolveEngineIcall(char const* symbol) {
  static std::unordered_map<std::string_view, void*> resolved;
  if (auto it = resolved.find(symbol); it != resolved.end()) return it->second;
  auto* fn = reinterpret_cast<void*>(il2cpp_functions::resolve_icall(symbol));
  resolved.emplace(symbol, fn);
  if (fn == nullptr) PaperLogger.warn("Vivify: '{}' is not present in this game build", symbol);
  return fn;
}

void Runtime::HandleSetRenderingSettings(CustomJSONData::CustomEventData* customEventData,
                                         rapidjson::Value const& json) {
  float duration = DurationBeatsToSeconds(ReadFloat(json, "duration").value_or(0.0f));
  Functions easing = ParseEasing(ReadStringView(json, "easing").value_or("easeLinear"));
  float startTime = customEventData->time;
  float songTime = CurrentSongTime();
  bool noDuration = duration <= 0.0f || startTime + duration <= songTime;
  std::vector<RenderSettingValue> settings;

  for (auto categoryKey : {"renderSettings"sv, "qualitySettings"sv, "xrSettings"sv}) {
    auto* categoryVal = ReadValuePtr(json, categoryKey);
    if (categoryVal == nullptr || !categoryVal->IsObject()) continue;
    for (auto it = categoryVal->MemberBegin(); it != categoryVal->MemberEnd(); ++it) {
      std::string key = it->name.GetString();
      // Realtime shadows are opt-in on Quest. A map that turns them on (RSIH,
      // Hold My Hand) puts every shadow-receiving shader on its shadowed
      // variant, which samples the screen-space shadow map -- and under the
      // Quest's multiview rendering that comes out fully shadowed, so the
      // game's note bodies drew black against a dark scene: arrows with no
      // body. They are also a heavy extra pass on a mobile GPU.
      if (categoryKey == "qualitySettings"sv && !GetMapRealtimeShadows() && key.rfind("shadow", 0) == 0) {
        static bool reported = false;
        if (!reported) {
          reported = true;
          PaperLogger.info("Vivify SetRenderingSettings: ignoring the map's realtime shadow settings ('{}' and "
                           "the rest); turn on 'Map Realtime Shadows' to let maps enable them", key);
        }
        continue;
      }
      ParseAndApplyRenderSetting(key, *categoryVal, it->value, settings, noDuration);
    }
  }
  if (!noDuration && !settings.empty()) {
    _renderSettingAnimations.emplace_back(ActiveRenderSettingAnimation{
        std::move(settings), startTime, duration, startTime + duration, easing});
  }

  if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify SetRenderingSettings: time={} durationSeconds={} immediate={}",
                     customEventData->time, duration, BoolText(noDuration));
    DumpRenderingState(fmt::format("after SetRenderingSettings at beat {}", customEventData->time));
  }
}

void Runtime::ParseAndApplyRenderSetting(std::string const& key, rapidjson::Value const& parent,
                                         rapidjson::Value const& val,
                                         std::vector<RenderSettingValue>& out, bool immediate) {
  auto typeIt = kRenderSettingTypes.find(key);
  if (typeIt == kRenderSettingTypes.end()) {
    if (GetVivifyDebugLogging()) {
      PaperLogger.warn("Vivify SetRenderingSettings: unsupported setting '{}'", key);
    }
    return;
  }
  float const initialProgress = immediate ? 1.0f : 0.0f;
  auto queue = [&](RenderSettingKind kind, PointDefinitionW point) {
    if (immediate) return;
    RenderSettingValue rsv;
    rsv.name = key;
    rsv.kind = kind;
    rsv.value = point;
    out.push_back(std::move(rsv));
  };
  switch (typeIt->second) {
    case SettingType::Color: {
      if (IsLiteralColorArray(val)) {
        if (auto color = ReadColorArray(val); color.has_value()) {
          ApplyRenderSettingColor(key, *color);
          UpdateEnvironmentLikePc();
        }
        return;
      }
      if (auto point = MakePointDefinition(parent, key, Tracks::ffi::WrapBaseValueType::Vec4); point.has_value()) {
        ApplyRenderSettingColor(key, VectorToColor(point->InterpolateVector4(initialProgress)));
        queue(RenderSettingKind::Color, *point);
      }
      return;
    }
    case SettingType::Float: {
      if (val.IsNumber()) {
        ApplyRenderSettingFloat(key, val.GetFloat());
        UpdateEnvironmentLikePc();
        return;
      }
      if (auto point = MakePointDefinition(parent, key, Tracks::ffi::WrapBaseValueType::Float); point.has_value()) {
        ApplyRenderSettingFloat(key, point->InterpolateLinear(initialProgress));
        queue(RenderSettingKind::Float, *point);
      }
      return;
    }
    case SettingType::Int: {
      if (val.IsNumber()) {
        ApplyRenderSettingInt(key, static_cast<int>(val.GetFloat()));
        UpdateEnvironmentLikePc();
        return;
      }
      if (val.IsString()) {
        std::string_view const name(val.GetString(), val.GetStringLength());
        if (auto named = ParseNamedSettingValue(key, name); named.has_value()) {
          ApplyRenderSettingInt(key, *named);
          UpdateEnvironmentLikePc();
        } else if (GetVivifyDebugLogging()) {
          PaperLogger.warn("Vivify SetRenderingSettings: '{}' has no value named '{}'", key, std::string(name));
        }
        return;
      }
      if (auto point = MakePointDefinition(parent, key, Tracks::ffi::WrapBaseValueType::Float); point.has_value()) {
        ApplyRenderSettingInt(key, static_cast<int>(point->InterpolateLinear(initialProgress)));
        queue(RenderSettingKind::Int, *point);
      }
      return;
    }
    case SettingType::Bool: {
      if (val.IsBool() || val.IsNumber()) {
        bool b = val.IsBool() ? val.GetBool() : val.GetFloat() != 0.0f;
        ApplyRenderSettingBool(key, b);
        UpdateEnvironmentLikePc();
        return;
      }
      if (auto point = MakePointDefinition(parent, key, Tracks::ffi::WrapBaseValueType::Float); point.has_value()) {
        ApplyRenderSettingBool(key, point->InterpolateLinear(initialProgress) >= 1.0f);
        queue(RenderSettingKind::Bool, *point);
      }
      return;
    }
    case SettingType::MaterialAsset:
    case SettingType::LightPrefab: {
      if (val.IsString()) {
        ApplyRenderSettingObject(key, val.GetString());
        UpdateEnvironmentLikePc();
      }
      return;
    }
  }
}

namespace {

// Reaches a QualitySettings/XRSettings property that bs-cordl does not declare.
//
// These are extern properties on the C# side, so they exist only as registered
// internal calls. Resolving one by name gives its address directly; a name this
// game build does not have resolves to null and the setting is then skipped,
// which is exactly what happened before this existed -- just now it is said
// once, by name, instead of being reported as an unsupported setting.
void* ResolveSettingIcall(std::string const& name, bool wantSetter) {
  auto ownerIt = kIcallSettingOwners.find(std::string_view(name));
  if (ownerIt == kIcallSettingOwners.end()) return nullptr;

  std::string const symbol =
      fmt::format("{}::{}_{}", ownerIt->second, wantSetter ? "set" : "get", name);

  static std::unordered_map<std::string, void*> resolved;
  if (auto it = resolved.find(symbol); it != resolved.end()) return it->second;

  auto* fn = reinterpret_cast<void*>(il2cpp_functions::resolve_icall(symbol.c_str()));
  resolved.emplace(symbol, fn);
  if (fn == nullptr) {
    PaperLogger.warn("Vivify SetRenderingSettings: '{}' is not present in this game build, "
                     "so that setting is ignored", symbol);
  } else if (GetVivifyDebugLogging()) {
    PaperLogger.info("Vivify SetRenderingSettings: resolved '{}'", symbol);
  }
  return fn;
}

bool TrySetSettingViaIcall(std::string const& name, int val) {
  auto* fn = ResolveSettingIcall(name, true);
  if (fn == nullptr) return false;
  reinterpret_cast<void (*)(int)>(fn)(val);
  return true;
}

bool TrySetSettingViaIcall(std::string const& name, float val) {
  auto* fn = ResolveSettingIcall(name, true);
  if (fn == nullptr) return false;
  reinterpret_cast<void (*)(float)>(fn)(val);
  return true;
}

bool TrySetSettingViaIcall(std::string const& name, bool val) {
  auto* fn = ResolveSettingIcall(name, true);
  if (fn == nullptr) return false;
  reinterpret_cast<void (*)(bool)>(fn)(val);
  return true;
}

// Reads the device's current value so RestoreRenderSettings can put it back
// when the level ends. The kind decides how the return value is interpreted,
// which is safe because the kind comes from the same table that decided how to
// write it.
std::optional<std::variant<float, UnityEngine::Color, int, bool, UnityEngine::Material*, UnityEngine::Light*>>
ReadSettingViaIcall(std::string const& name, RenderSettingKind kind) {
  auto* fn = ResolveSettingIcall(name, false);
  if (fn == nullptr) return std::nullopt;
  switch (kind) {
    case RenderSettingKind::Int:
      return reinterpret_cast<int (*)()>(fn)();
    case RenderSettingKind::Float:
      return reinterpret_cast<float (*)()>(fn)();
    case RenderSettingKind::Bool:
      return reinterpret_cast<bool (*)()>(fn)();
    default:
      return std::nullopt;
  }
}

void RawApplyRenderSettingFloat(std::string const& name, float val) {
  if (name == "fogDensity") UnityEngine::RenderSettings::set_fogDensity(val);
  else if (name == "fogStartDistance") UnityEngine::RenderSettings::set_fogStartDistance(val);
  else if (name == "fogEndDistance") UnityEngine::RenderSettings::set_fogEndDistance(val);
  else if (name == "ambientIntensity") UnityEngine::RenderSettings::set_ambientIntensity(val);
  else if (name == "reflectionIntensity") UnityEngine::RenderSettings::set_reflectionIntensity(val);
  else if (name == "haloStrength") UnityEngine::RenderSettings::set_haloStrength(val);
  else if (name == "flareStrength") UnityEngine::RenderSettings::set_flareStrength(val);
  else if (name == "flareFadeSpeed") UnityEngine::RenderSettings::set_flareFadeSpeed(val);
  else TrySetSettingViaIcall(name, val);
}
void RawApplyRenderSettingColor(std::string const& name, UnityEngine::Color val) {
  if (name == "fogColor") UnityEngine::RenderSettings::set_fogColor(val);
  else if (name == "ambientLight") UnityEngine::RenderSettings::set_ambientLight(val);
  else if (name == "ambientSkyColor") UnityEngine::RenderSettings::set_ambientSkyColor(val);
  else if (name == "ambientEquatorColor") UnityEngine::RenderSettings::set_ambientEquatorColor(val);
  else if (name == "ambientGroundColor") UnityEngine::RenderSettings::set_ambientGroundColor(val);
  else if (name == "subtractiveShadowColor") UnityEngine::RenderSettings::set_subtractiveShadowColor(val);
}
void RawApplyRenderSettingBool(std::string const& name, bool val) {
  if (name == "fog") UnityEngine::RenderSettings::set_fog(val);
  else TrySetSettingViaIcall(name, val);
}
void RawApplyRenderSettingInt(std::string const& name, int val) {
  if (name == "antiAliasing") UnityEngine::QualitySettings::set_antiAliasing(val);
  else if (name == "fogMode") UnityEngine::RenderSettings::set_fogMode(static_cast<UnityEngine::FogMode>(val));
  else if (name == "ambientMode") UnityEngine::RenderSettings::set_ambientMode(static_cast<UnityEngine::Rendering::AmbientMode>(val));
  else if (name == "defaultReflectionMode") UnityEngine::RenderSettings::set_defaultReflectionMode(static_cast<UnityEngine::Rendering::DefaultReflectionMode>(val));
  else if (name == "defaultReflectionResolution") UnityEngine::RenderSettings::set_defaultReflectionResolution(val);
  else if (name == "reflectionBounces") UnityEngine::RenderSettings::set_reflectionBounces(val);
  else TrySetSettingViaIcall(name, val);
}
void RawApplySavedRenderSetting(SavedRenderSetting const& s) {
  switch (s.kind) {
    case RenderSettingKind::Float:
      if (std::holds_alternative<float>(s.saved)) RawApplyRenderSettingFloat(s.name, std::get<float>(s.saved));
      break;
    case RenderSettingKind::Color:
      if (std::holds_alternative<UnityEngine::Color>(s.saved)) RawApplyRenderSettingColor(s.name, std::get<UnityEngine::Color>(s.saved));
      break;
    case RenderSettingKind::Bool:
      if (std::holds_alternative<bool>(s.saved)) RawApplyRenderSettingBool(s.name, std::get<bool>(s.saved));
      break;
    case RenderSettingKind::Int:
      if (std::holds_alternative<int>(s.saved)) RawApplyRenderSettingInt(s.name, std::get<int>(s.saved));
      break;
    case RenderSettingKind::Material:
      if (std::holds_alternative<UnityEngine::Material*>(s.saved)) {
        UnityEngine::RenderSettings::set_skybox(std::get<UnityEngine::Material*>(s.saved));
      }
      break;
    case RenderSettingKind::Light:
      if (std::holds_alternative<UnityEngine::Light*>(s.saved)) {
        UnityEngine::RenderSettings::set_sun(std::get<UnityEngine::Light*>(s.saved));
      }
      break;
  }
}
}

void Runtime::SaveRenderSetting(std::string const& name, RenderSettingKind kind) {
  for (auto const& s : _savedRenderSettings) {
    if (s.name == name) return;
  }
  SavedRenderSetting saved;
  saved.name = name;
  saved.kind = kind;
  if (name == "fog") saved.saved = UnityEngine::RenderSettings::get_fog();
  else if (name == "fogColor") saved.saved = UnityEngine::RenderSettings::get_fogColor();
  else if (name == "fogDensity") saved.saved = UnityEngine::RenderSettings::get_fogDensity();
  else if (name == "fogStartDistance") saved.saved = UnityEngine::RenderSettings::get_fogStartDistance();
  else if (name == "fogEndDistance") saved.saved = UnityEngine::RenderSettings::get_fogEndDistance();
  else if (name == "fogMode") saved.saved = static_cast<int>(UnityEngine::RenderSettings::get_fogMode());
  else if (name == "ambientIntensity") saved.saved = UnityEngine::RenderSettings::get_ambientIntensity();
  else if (name == "ambientLight") saved.saved = UnityEngine::RenderSettings::get_ambientLight();
  else if (name == "ambientMode") saved.saved = static_cast<int>(UnityEngine::RenderSettings::get_ambientMode());
  else if (name == "reflectionIntensity") saved.saved = UnityEngine::RenderSettings::get_reflectionIntensity();
  else if (name == "reflectionBounces") saved.saved = UnityEngine::RenderSettings::get_reflectionBounces();
  else if (name == "defaultReflectionMode") saved.saved = static_cast<int>(UnityEngine::RenderSettings::get_defaultReflectionMode());
  else if (name == "defaultReflectionResolution") saved.saved = UnityEngine::RenderSettings::get_defaultReflectionResolution();
  else if (name == "haloStrength") saved.saved = UnityEngine::RenderSettings::get_haloStrength();
  else if (name == "flareStrength") saved.saved = UnityEngine::RenderSettings::get_flareStrength();
  else if (name == "flareFadeSpeed") saved.saved = UnityEngine::RenderSettings::get_flareFadeSpeed();
  else if (name == "ambientSkyColor") saved.saved = UnityEngine::RenderSettings::get_ambientSkyColor();
  else if (name == "ambientEquatorColor") saved.saved = UnityEngine::RenderSettings::get_ambientEquatorColor();
  else if (name == "ambientGroundColor") saved.saved = UnityEngine::RenderSettings::get_ambientGroundColor();
  else if (name == "subtractiveShadowColor") saved.saved = UnityEngine::RenderSettings::get_subtractiveShadowColor();
  else if (name == "skybox") saved.saved = UnityEngine::RenderSettings::get_skybox().unsafePtr();
  else if (name == "sun") saved.saved = UnityEngine::RenderSettings::get_sun().unsafePtr();
  else if (name == "antiAliasing") saved.saved = UnityEngine::QualitySettings::get_antiAliasing();
  else if (auto current = ReadSettingViaIcall(name, kind); current.has_value()) saved.saved = *current;
  // Nothing knows how to read this one back, so nothing may write it either --
  // a setting that cannot be restored would leak into the next level.
  else return;
  _savedRenderSettings.push_back(std::move(saved));
}

void Runtime::ApplyRenderSettingFloat(std::string const& name, float val) {
  SaveRenderSetting(name, RenderSettingKind::Float);
  RawApplyRenderSettingFloat(name, val);
  for (auto& s : _currentRenderSettings) {
    if (s.name == name) { s.saved = val; return; }
  }
  _currentRenderSettings.push_back(SavedRenderSetting{name, RenderSettingKind::Float, val});
}

void Runtime::ApplyRenderSettingColor(std::string const& name, UnityEngine::Color val) {
  SaveRenderSetting(name, RenderSettingKind::Color);
  RawApplyRenderSettingColor(name, val);
  for (auto& s : _currentRenderSettings) {
    if (s.name == name) { s.saved = val; return; }
  }
  _currentRenderSettings.push_back(SavedRenderSetting{name, RenderSettingKind::Color, val});
}

void Runtime::ApplyRenderSettingBool(std::string const& name, bool val) {
  SaveRenderSetting(name, RenderSettingKind::Bool);
  RawApplyRenderSettingBool(name, val);
  for (auto& s : _currentRenderSettings) {
    if (s.name == name) { s.saved = val; return; }
  }
  _currentRenderSettings.push_back(SavedRenderSetting{name, RenderSettingKind::Bool, val});
}

void Runtime::ApplyRenderSettingInt(std::string const& name, int val) {
  SaveRenderSetting(name, RenderSettingKind::Int);
  RawApplyRenderSettingInt(name, val);
  for (auto& s : _currentRenderSettings) {
    if (s.name == name) { s.saved = val; return; }
  }
  _currentRenderSettings.push_back(SavedRenderSetting{name, RenderSettingKind::Int, val});
}

void Runtime::ApplyRenderSettingObject(std::string const& name, std::string const& assetOrId) {
  if (name == "skybox") {
    auto* material = GetAssetAs<UnityEngine::Material>(assetOrId);
    if (!IsAlive(material)) {
      if (GetVivifyDebugLogging()) {
        PaperLogger.warn("Vivify SetRenderingSettings: skybox material '{}' not found", assetOrId);
      }
      return;
    }
    SaveRenderSetting(name, RenderSettingKind::Material);
    RepairMaterialShader(material, "skybox");
    UnityEngine::RenderSettings::set_skybox(material);
    for (auto& s : _currentRenderSettings) {
      if (s.name == name) { s.saved = material; return; }
    }
    _currentRenderSettings.push_back(SavedRenderSetting{name, RenderSettingKind::Material, material});
    return;
  }
  if (name == "sun") {

    auto prefabIt = _livePrefabs.find(assetOrId);
    if (prefabIt == _livePrefabs.end() || !IsAlive(prefabIt->second.gameObject)) {
      if (GetVivifyDebugLogging()) {
        PaperLogger.warn("Vivify SetRenderingSettings: sun prefab '{}' not found", assetOrId);
      }
      return;
    }
    UnityEngine::Light* directional = nullptr;
    auto lights = prefabIt->second.gameObject->GetComponentsInChildren<UnityEngine::Light*>(true);
    for (int i = 0; i < lights.size(); i++) {
      if (IsAlive(lights[i]) && lights[i]->get_type().value__ == UnityEngine::LightType::Directional.value__) {
        directional = lights[i];
        break;
      }
    }
    if (directional == nullptr) {
      if (GetVivifyDebugLogging()) {
        PaperLogger.warn("Vivify SetRenderingSettings: sun prefab '{}' has no directional light", assetOrId);
      }
      return;
    }
    SaveRenderSetting(name, RenderSettingKind::Light);
    UnityEngine::RenderSettings::set_sun(directional);
    for (auto& s : _currentRenderSettings) {
      if (s.name == name) { s.saved = directional; return; }
    }
    _currentRenderSettings.push_back(SavedRenderSetting{name, RenderSettingKind::Light, directional});
  }
}

void Runtime::UpdateRenderSettingAnimations() {
  if (_renderSettingAnimations.empty()) return;
  float const songTime = CurrentSongTime();
  auto write = _renderSettingAnimations.begin();
  for (auto read = _renderSettingAnimations.begin(); read != _renderSettingAnimations.end(); ++read) {
    float const progress = AnimationProgress(read->startTime, read->duration, read->easing, songTime);
    for (auto const& s : read->settings) {
      if (!std::holds_alternative<PointDefinitionW>(s.value)) continue;
      auto const& point = std::get<PointDefinitionW>(s.value);
      switch (s.kind) {
        case RenderSettingKind::Float:
          ApplyRenderSettingFloat(s.name, point.InterpolateLinear(progress));
          break;
        case RenderSettingKind::Color:
          ApplyRenderSettingColor(s.name, VectorToColor(point.InterpolateVector4(progress)));
          break;
        case RenderSettingKind::Int:
          ApplyRenderSettingInt(s.name, static_cast<int>(point.InterpolateLinear(progress)));
          break;
        case RenderSettingKind::Bool:
          ApplyRenderSettingBool(s.name, point.InterpolateLinear(progress) >= 1.0f);
          break;
        default:
          break;
      }
    }
    if (songTime < read->endTime) {
      if (write != read) *write = std::move(*read);
      ++write;
    }
  }
  _renderSettingAnimations.erase(write, _renderSettingAnimations.end());
}

void Runtime::RestoreRenderSettings() {
  for (auto const& s : _savedRenderSettings) {
    RawApplySavedRenderSetting(s);
  }
}

void Runtime::ReapplyCurrentRenderSettings() {
  for (auto const& s : _currentRenderSettings) {
    RawApplySavedRenderSetting(s);
  }
}

// ---------------------------------------------------------------------------
// Render state dump (naomi.7, Debug logging only)
//
// What the scene is actually lit and shadowed with, read back from the
// engine: for comparing with PC when a map's lighting differs (Lost in Data's
// pure-black shadows). Written after the time-0 catch-up, after each
// SetRenderingSettings event, and at level end before the settings are put
// back, by which time an environment update has settled.

namespace {

std::string ColorText(UnityEngine::Color c) {
  return fmt::format("({:.3f},{:.3f},{:.3f},{:.3f})", c.r, c.g, c.b, c.a);
}

std::string ObjectPath(UnityEngine::Transform* transform) {
  std::vector<std::string> parts;
  for (int depth = 0; IsManagedAlive(transform) && depth < 12; depth++) {
    parts.push_back(ToStdString(transform->get_name()));
    transform = transform->get_parent();
  }
  std::string path;
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) path += (path.empty() ? "" : "/") + *it;
  return path;
}

template <typename T>
std::string StaticIcallText(char const* symbol) {
  auto* fn = reinterpret_cast<T (*)()>(ResolveEngineIcall(symbol));
  if (fn == nullptr) return "?";
  return fmt::format("{}", fn());
}

template <typename T>
std::string InstanceIcallText(char const* symbol, UnityEngine::Object* self) {
  auto* fn = reinterpret_cast<T (*)(UnityEngine::Object*)>(ResolveEngineIcall(symbol));
  if (fn == nullptr || !IsManagedAlive(self)) return "?";
  return fmt::format("{}", fn(self));
}

}  // namespace

void Runtime::DumpRenderingState(std::string_view reason) {
  try {
    using UnityEngine::RenderSettings;
    auto const probe = RenderSettings::get_ambientProbe();
    auto* skybox = RenderSettings::get_skybox().unsafePtr();
    auto* sun = RenderSettings::get_sun().unsafePtr();
    // The probe's L0 (constant) coefficients, and the average ambient colour
    // they make (times the basis constant 0.2821): near black there means the
    // shadowed side of everything gets no ambient light at all.
    float constexpr kY00 = 0.282095f;
    PaperLogger.info(
        "Vivify render state ({}): RenderSettings ambientMode={} ambientSky={} ambientEquator={} ambientGround={} "
        "ambientLight={} ambientIntensity={:.3f} ambientSkyboxAmount={:.3f} ambientProbe L0=({:.4f},{:.4f},{:.4f}) "
        "average=({:.4f},{:.4f},{:.4f}) skybox='{}' shader '{}' sun='{}' fog={} fogMode={} fogColor={} fogDensity={:.4f} "
        "fogStart={:.2f} fogEnd={:.2f} defaultReflectionMode={} defaultReflectionResolution={} "
        "reflectionIntensity={:.3f} reflectionBounces={} subtractiveShadowColor={} haloStrength={:.3f} "
        "flareStrength={:.3f}",
        reason, static_cast<int>(RenderSettings::get_ambientMode().value__), ColorText(RenderSettings::get_ambientSkyColor()),
        ColorText(RenderSettings::get_ambientEquatorColor()), ColorText(RenderSettings::get_ambientGroundColor()),
        ColorText(RenderSettings::get_ambientLight()), RenderSettings::get_ambientIntensity(),
        RenderSettings::get_ambientSkyboxAmount(), probe.shr0, probe.shg0, probe.shb0, probe.shr0 * kY00,
        probe.shg0 * kY00, probe.shb0 * kY00, IsAlive(skybox) ? ToStdString(skybox->get_name()) : std::string("none"),
        IsAlive(skybox) ? ShaderNameForLog(skybox->get_shader().unsafePtr()) : std::string("-"),
        IsAlive(sun) ? ObjectPath(sun->get_transform()) : std::string("none"), BoolText(RenderSettings::get_fog()),
        static_cast<int>(RenderSettings::get_fogMode().value__), ColorText(RenderSettings::get_fogColor()),
        RenderSettings::get_fogDensity(), RenderSettings::get_fogStartDistance(), RenderSettings::get_fogEndDistance(),
        static_cast<int>(RenderSettings::get_defaultReflectionMode().value__),
        RenderSettings::get_defaultReflectionResolution(), RenderSettings::get_reflectionIntensity(),
        RenderSettings::get_reflectionBounces(), ColorText(RenderSettings::get_subtractiveShadowColor()),
        RenderSettings::get_haloStrength(), RenderSettings::get_flareStrength());

    std::string levelName = "?";
    int level = -1;
    if (auto* getLevel = reinterpret_cast<int (*)()>(ResolveEngineIcall("UnityEngine.QualitySettings::GetQualityLevel"))) {
      level = getLevel();
      if (auto* getNames = reinterpret_cast<Array<StringW>* (*)()>(
              ResolveEngineIcall("UnityEngine.QualitySettings::get_names"))) {
        ArrayW<StringW> names(getNames());
        if (names && level >= 0 && level < static_cast<int>(names.size())) levelName = ToStdString(names[level]);
      }
    }
    PaperLogger.info(
        "Vivify render state ({}): QualitySettings level={} '{}' shadows={} shadowResolution={} shadowProjection={} "
        "shadowCascades={} shadowCascade2Split={} shadowDistance={} shadowNearPlaneOffset={} shadowmaskMode={} "
        "pixelLightCount={} antiAliasing={} anisotropicFiltering={} lodBias={} globalTextureMipmapLimit={} "
        "realtimeReflectionProbes={} softParticles={} eyeTextureResolutionScale={}",
        reason, level, levelName, StaticIcallText<int>("UnityEngine.QualitySettings::get_shadows"),
        StaticIcallText<int>("UnityEngine.QualitySettings::get_shadowResolution"),
        StaticIcallText<int>("UnityEngine.QualitySettings::get_shadowProjection"),
        StaticIcallText<int>("UnityEngine.QualitySettings::get_shadowCascades"),
        StaticIcallText<float>("UnityEngine.QualitySettings::get_shadowCascade2Split"),
        StaticIcallText<float>("UnityEngine.QualitySettings::get_shadowDistance"),
        StaticIcallText<float>("UnityEngine.QualitySettings::get_shadowNearPlaneOffset"),
        StaticIcallText<int>("UnityEngine.QualitySettings::get_shadowmaskMode"),
        StaticIcallText<int>("UnityEngine.QualitySettings::get_pixelLightCount"),
        UnityEngine::QualitySettings::get_antiAliasing(),
        StaticIcallText<int>("UnityEngine.QualitySettings::get_anisotropicFiltering"),
        StaticIcallText<float>("UnityEngine.QualitySettings::get_lodBias"),
        StaticIcallText<int>("UnityEngine.QualitySettings::get_globalTextureMipmapLimit"),
        StaticIcallText<bool>("UnityEngine.QualitySettings::get_realtimeReflectionProbes"),
        StaticIcallText<bool>("UnityEngine.QualitySettings::get_softParticles"),
        StaticIcallText<float>("UnityEngine.XR.XRSettings::get_eyeTextureResolutionScale"));

    auto lights = UnityEngine::Object::FindObjectsOfType<UnityEngine::Light*>(true);
    PaperLogger.info("Vivify render state ({}): {} light(s)", reason, lights.size());
    for (auto* light : lights) {
      if (!IsAlive(light)) continue;
      auto const baking = light->get_bakingOutput();
      PaperLogger.info(
          "Vivify render state ({}): light '{}' enabled={} active={} type={} color={} intensity={:.3f} shadows={} "
          "shadowStrength={} shadowResolution={} shadowCustomResolution={} shadowBias={} shadowNormalBias={} "
          "shadowNearPlane={} renderMode={} cullingMask=0x{:x} baked={} bakeType={}",
          reason, ObjectPath(light->get_transform()), BoolText(light->get_enabled()),
          BoolText(light->get_gameObject()->get_activeInHierarchy()), static_cast<int>(light->get_type().value__),
          ColorText(light->get_color()), light->get_intensity(), static_cast<int>(light->get_shadows().value__),
          InstanceIcallText<float>("UnityEngine.Light::get_shadowStrength", light),
          InstanceIcallText<int>("UnityEngine.Light::get_shadowResolution", light),
          InstanceIcallText<int>("UnityEngine.Light::get_shadowCustomResolution", light),
          InstanceIcallText<float>("UnityEngine.Light::get_shadowBias", light),
          InstanceIcallText<float>("UnityEngine.Light::get_shadowNormalBias", light),
          InstanceIcallText<float>("UnityEngine.Light::get_shadowNearPlane", light),
          InstanceIcallText<int>("UnityEngine.Light::get_renderMode", light),
          static_cast<uint32_t>([&] {
            auto* fn = reinterpret_cast<int (*)(UnityEngine::Object*)>(
                ResolveEngineIcall("UnityEngine.Light::get_cullingMask"));
            return fn != nullptr ? fn(light) : 0;
          }()),
          BoolText(baking.isBaked), static_cast<int>(baking.lightmapBakeType.value__));
    }

    // Textures on the live prefabs' materials, largest first: a ground texture
    // with one mip level, or a low anisoLevel, is what grain comes from.
    std::unordered_map<UnityEngine::Texture*, int64_t> textures;
    for (auto const& [id, prefab] : _livePrefabs) {
      if (!IsAlive(prefab.gameObject)) continue;
      for (auto* renderer : prefab.gameObject->GetComponentsInChildren<UnityEngine::Renderer*>(true)) {
        if (!IsAlive(renderer)) continue;
        for (auto material : renderer->get_sharedMaterials()) {
          if (!IsAlive(material.unsafePtr())) continue;
          for (int32_t nameId : material->GetTexturePropertyNameIDs()) {
            auto* texture = material->GetTexture(nameId).unsafePtr();
            if (!IsAlive(texture)) continue;
            textures.emplace(texture, static_cast<int64_t>(texture->get_width()) * texture->get_height());
          }
        }
      }
    }
    std::vector<std::pair<UnityEngine::Texture*, int64_t>> byArea(textures.begin(), textures.end());
    std::sort(byArea.begin(), byArea.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
    PaperLogger.info("Vivify render state ({}): {} texture(s) on live prefabs' materials{}", reason, byArea.size(),
                     byArea.size() > 40 ? ", the 40 largest follow" : "");
    for (size_t i = 0; i < byArea.size() && i < 40; i++) {
      auto* texture = byArea[i].first;
      PaperLogger.info("Vivify render state ({}): texture '{}' {}x{} mips={} filter={} aniso={} mipMapBias={} wrap={}",
                       reason, ToStdString(texture->get_name()), texture->get_width(), texture->get_height(),
                       texture->get_mipmapCount(), static_cast<int>(texture->get_filterMode().value__),
                       texture->get_anisoLevel(), InstanceIcallText<float>("UnityEngine.Texture::get_mipMapBias", texture),
                       static_cast<int>(texture->get_wrapMode().value__));
    }
  } catch (std::exception const& ex) {
    PaperLogger.warn("Vivify render state ({}): the dump stopped: {}", reason, ex.what());
  } catch (...) {
    PaperLogger.warn("Vivify render state ({}): the dump stopped on a non-std exception", reason);
  }
}

}
