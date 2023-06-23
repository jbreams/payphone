#include "yaml_persisted_obj.hpp"

namespace {
bool yaml_hasUnread(const pj::ContainerNode *cn) {
  auto reader = reinterpret_cast<YamlReader *>(cn->data.data1);
  return reader->has_next(cn);
}

std::string yaml_unreadName(const pj::ContainerNode *cn) {
  auto reader = reinterpret_cast<YamlReader *>(cn->data.data1);
  return reader->node(cn).begin()->first.as<std::string>();
}

template <typename Ret>
Ret yaml_ReadScalar(const pj::ContainerNode *cn, const std::string &name) {
  auto reader = reinterpret_cast<YamlReader *>(cn->data.data1);
  auto &top = reader->node(cn);
  if constexpr (std::is_same_v<YAML::Node, Ret>) {
    if (top.IsSequence() && top.size() > 0) {
      auto ret = top[0];
      top.remove(0);
      return ret;
    }
  }
  if (const auto &&node = top[name]; node.IsDefined()) {
    auto ret = node.as<Ret>();
    top.remove(node);
    return ret;
  }

  return Ret{};
}

float yaml_readNumber(const pj::ContainerNode *cn, const std::string &name) {
  return yaml_ReadScalar<float>(cn, name);
}

bool yaml_readBool(const pj::ContainerNode *cn, const std::string &name) {
  return yaml_ReadScalar<bool>(cn, name);
}

std::string yaml_readString(const pj::ContainerNode *cn,
                            const std::string &name) {
  return yaml_ReadScalar<std::string>(cn, name);
}

pj::StringVector yaml_readStringVector(const pj::ContainerNode *cn,
                                       const std::string &name) {
  return yaml_ReadScalar<std::vector<std::string>>(cn, name);
}

pj::ContainerNode yaml_readContainer(const pj::ContainerNode *cn,
                                     const std::string &name) {
  auto reader = reinterpret_cast<YamlReader *>(cn->data.data1);
  if (name == reader->name(cn)) {
    return *cn;
  }
  reader->nodes.push_back({name, yaml_ReadScalar<YAML::Node>(cn, name)});
  return reader->make_pj_container_node(reader->nodes.size() - 1);
}

pj::ContainerNode yaml_readArray(const pj::ContainerNode *cn,
                                 const std::string &name) {
  auto reader = reinterpret_cast<YamlReader *>(cn->data.data1);
  reader->nodes.push_back({"", yaml_ReadScalar<YAML::Node>(cn, name)});
  return reader->make_pj_container_node(reader->nodes.size() - 1);
}

static struct pj::container_node_op yaml_ops = [] {
  pj::container_node_op ret = {};
  ret.hasUnread = yaml_hasUnread;
  ret.unreadName = yaml_unreadName;
  ret.readNumber = yaml_readNumber;
  ret.readBool = yaml_readBool;
  ret.readString = yaml_readString;
  ret.readStringVector = yaml_readStringVector;
  ret.readContainer = yaml_readContainer;
  ret.readArray = yaml_readArray;
  return ret;
}();
} // namespace

pj::ContainerNode YamlReader::make_pj_container_node(size_t index) {
  pj::ContainerNode ret;
  ret.op = &yaml_ops;
  ret.data.data1 = this;
  ret.data.data2 = reinterpret_cast<void *>(index);
  return ret;
}
