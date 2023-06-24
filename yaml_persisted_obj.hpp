#include <yaml-cpp/yaml.h>

#include <pjsua2.hpp>

class YamlReader {
public:
  explicit YamlReader(YAML::Node top, std::string top_name, YAML::Node default_values)
      : top_node(std::move(top)) {
    nodes.push_back({std::move(top_name), {top_node, default_values}});
    pj_top_node = make_pj_container_node(0);
  }

  bool has_next(const pj::ContainerNode *cn) {
    return node(cn).begin() != node(cn).end();
  }

  const std::string &name(const pj::ContainerNode *cn) {
    size_t index = reinterpret_cast<size_t>(cn->data.data2);
    return nodes.at(index).first;
  }

  YAML::Node &node(const pj::ContainerNode *cn) {
    size_t index = reinterpret_cast<size_t>(cn->data.data2);
    return nodes.at(index).second.first;
  }

  YAML::Node& default_node(const pj::ContainerNode *cn) {
    size_t index = reinterpret_cast<size_t>(cn->data.data2);
    return nodes.at(index).second.second;
  }

  pj::ContainerNode make_pj_container_node(size_t index);

  pj::ContainerNode &get_pj_container_node() { return pj_top_node; }

  pj::ContainerNode pj_top_node;
  YAML::Node top_node;

  std::vector<std::pair<std::string, std::pair<YAML::Node, YAML::Node>>> nodes;
};
