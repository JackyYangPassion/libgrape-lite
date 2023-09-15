#ifndef EXAMPLES_ANALYTICAL_APPS_ASYNC_BFS_H_
#define EXAMPLES_ANALYTICAL_APPS_ASYNC_BFS_H_
#include "app_config.h"
#include "grape_gpu/grape_gpu.h"


namespace grape_gpu {
template <typename FRAG_T>
class AsyncBFSContext : public grape::VoidContext<FRAG_T> {
 public:
  using vid_t = typename FRAG_T::vid_t;
  using oid_t = typename FRAG_T::oid_t;
  using vertex_t = typename FRAG_T::vertex_t;
  using depth_t = uint32_t;
  using msg_t = vertex_t;

  explicit AsyncBFSContext(const FRAG_T& frag) : grape::VoidContext<FRAG_T>(frag) {}

  void Init(AsyncGPUMessageManager<msg_t>& messages, AppConfig app_config, oid_t src_id, 
            size_t chunk_size, size_t chunk_num, size_t capacity) {
    auto& frag = this->fragment();
    auto vertices = frag.Vertices();
    auto iv = frag.InnerVertices();
    auto ov = frag.OuterVertices();

    this->src_id = src_id;
    this->lb = app_config.lb;
    depth.Init(vertices, std::numeric_limits<depth_t>::max());
    depth.H2D();
    in_q.Init(iv.size());
    out_q_local.Init(iv.size());

    messages.InitDriver(chunk_num, chunk_size, capacity);
  }

  void Output(std::ostream& os) override {
    auto& frag = this->fragment();
    auto iv = frag.InnerVertices();

    depth.D2H();

    for (auto v : iv) {
      os << frag.GetId(v) << " " << depth[v] << std::endl;
    }
  }

  oid_t src_id{};
  LoadBalancing lb{};
  depth_t curr_depth{};
  VertexArray<depth_t, vid_t> depth;
  Queue<vertex_t, vid_t> in_q, out_q_local;
};

template <typename FRAG_T>
class AsyncBFS : public AsyncGPUAppBase<FRAG_T, AsyncBFSContext<FRAG_T>>,
                 public ParallelEngine {
 public:
  INSTALL_ASYNC_GPU_WORKER(AsyncBFS<FRAG_T>, AsyncBFSContext<FRAG_T>, FRAG_T)
  using depth_t = typename context_t::depth_t;
  using dev_fragment_t = typename fragment_t::device_t;
  using vid_t = typename fragment_t::vid_t;
  using edata_t = typename fragment_t::edata_t;
  using vertex_t = typename dev_fragment_t::vertex_t;
  using nbr_t = typename dev_fragment_t::nbr_t;

  void PEval(const fragment_t& frag, context_t& ctx,
             message_manager_t& messages) {
    auto src_id = ctx.src_id;
    vertex_t source;
    bool native_source = frag.GetInnerVertex(src_id, source);

    if (native_source) {
      LaunchKernel(
          messages.stream(),
          [=] __device__(dev_fragment_t d_frag,
                         dev::VertexArray<depth_t, vid_t> depth,
                         dev::Queue<vertex_t, vid_t> in_q) {
            auto tid = TID_1D;

            if (tid == 0) {
              depth[source] = 0;
              in_q.Append(source);
            }
          },
          frag.DeviceObject(), ctx.depth.DeviceObject(),
          ctx.in_q.DeviceObject());
    }
  }

  // consume PCQueue then append incomming message into local queue
  void Unpack(const fragment_t& frag, context_t& ctx,
              message_manager_t& messages) {
    auto d_frag = frag.DeviceObject();
    auto d_depth = ctx.depth.DeviceObject();
    auto& in_q = ctx.in_q;
    auto d_in_q = in_q.DeviceObject();
    auto curr_depth = ctx.curr_depth;

    messages.template ParallelProcess<dev_fragment_t, grape::EmptyType>(
      d_frag, [=] __device__(vertex_t v) mutable {
        assert(d_frag.IsInnerVertex(v));

        if (curr_depth < d_depth[v]) {
          d_depth[v] = curr_depth;
          d_in_q.AppendWarp(v);
        }
    });
  }

  // process local queue then genearte next level local queue
  void Compute(const fragment_t& frag, context_t& ctx,
               message_manager_t& messages) {
    auto d_frag = frag.DeviceObject();
    auto d_depth = ctx.depth.DeviceObject();
    auto& in_q = ctx.in_q;
    auto d_in_q = in_q.DeviceObject();
    auto& out_q_local = ctx.out_q_local;
    auto d_out_q_local = out_q_local.DeviceObject();
    auto curr_depth = ctx.curr_depth;
    auto next_depth = curr_depth + 1;
    auto& stream = messages.stream();
    auto d_mm = messages.DeviceObject();

    // messages.Register(+in_size);
    auto in_size = in_q.size(stream);
    WorkSourceArray<vertex_t> ws_in(in_q.data(), in_size);

    ForEachOutgoingEdge(
        stream, d_frag, ws_in,
        [=] __device__(const vertex_t& u, const nbr_t& nbr) mutable {
          vertex_t v = nbr.get_neighbor();

          if (next_depth < d_depth[v]) {
            d_depth[v] = next_depth;

            if (d_frag.IsInnerVertex(v)) {
              d_out_q_local.AppendWarp(v);
            } else {
              d_mm.SyncStateOnOuterVertex(d_frag, v);
            }
          }
        },
        ctx.lb);
    auto local_out_size = out_q_local.size(stream);
    // messages.Register(-local_out_size);
    in_q.Clear(stream);
    out_q_local.Swap(in_q);
    ctx.curr_depth = next_depth;
  }

};
}  // namespace grape_gpu
#endif  // EXAMPLES_ANALYTICAL_APPS_ASYNC_BFS_H_