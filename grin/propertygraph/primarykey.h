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

// This header file is not available for libgrape-lite.

#ifndef GRIN_PROPERTY_GRAPH_PRIMARY_KEY_H_
#define GRIN_PROPERTY_GRAPH_PRIMARY_KEY_H_

#include "grin/predefine.h"

#ifdef WITH_VERTEX_PRIMARTY_KEYS

PropertyList get_primary_keys(const Graph);

Vertex get_vertex_from_primay_keys(const Graph, const Row);

#endif

#endif  // GRIN_PROPERTY_GRAPH_PRIMARY_KEY_H_