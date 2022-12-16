/** Copyright 2020 Alibaba Group Holding Limited.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

// TODO: implement structure.cc

#ifndef GRIN_TOPOLOGY_STRUCTURE_H_
#define GRIN_TOPOLOGY_STRUCTURE_H_

#include "grin/predefine.h"

Direction get_edge_direction(const Graph, const Edge);

#ifdef WITH_EDGE_SRC
Vertex get_edge_src(const Graph, const Edge);
#endif

#ifdef WITH_EDGE_DST
Vertex get_edge_dst(const Graph, const Edge);
#endif

#ifdef WITH_EDGE_WEIGHT
DataType get_edge_weight_type(const Graph, const Edge);

void* get_edge_weight_value(const Graph, Edge);
#endif

#ifdef WITH_VERTEX_DATA
DataType get_vertex_data_type(const Graph, const Vertex);

void* get_vertex_data_value(const Graph, Vertex);

void set_vertex_data_value(const Graph, Vertex, const void*);
#endif

#endif  // GRIN_TOPOLOGY_STRUCTURE_H_
