
#ifndef GRAPEGPU_EXAMPLES_ANALYTICAL_APPS_SSSP_SSSP_OLD_H_
#define GRAPEGPU_EXAMPLES_ANALYTICAL_APPS_SSSP_SSSP_OLD_H_
#include "app_config.h"
#include "grape_gpu/grape_gpu.h"

namespace grape_gpu {
template <typename FRAG_T>
class SSSPOldContext : public grape::VoidContext<FRAG_T> {
 public:
  using vid_t = typename FRAG_T::vid_t;
  using oid_t = typename FRAG_T::oid_t;
  using vertex_t = typename FRAG_T::vertex_t;
  using dist_t = uint32_t;
  explicit SSSPOldContext(const FRAG_T& frag)
      : grape::VoidContext<FRAG_T>(frag) {}

  void Init(GPUMessageManager& messages, AppConfig app_config, oid_t src_id,
            int init_prio) {
    auto& frag = this->fragment();
    auto vertices = frag.Vertices();
    auto iv = frag.InnerVertices();
    auto ov = frag.OuterVertices();

    this->src_id = src_id;
    this->lb = app_config.lb;
    dist.Init(vertices, std::numeric_limits<dist_t>::max());
    dist.H2D();

    auto in_cap = app_config.wl_alloc_factor_in * frag.GetEdgeNum();
    auto local_out_cap =
        (frag.fnum() == 1 ? app_config.wl_alloc_factor_in
                          : app_config.wl_alloc_factor_out_local) *
        frag.GetEdgeNum();
    auto remote_out_cap = ov.size();

    tmp_q.Init(iv.size());
    in_q.Init(iv);
    out_q_local_near.Init(iv);
    out_q_local_far.Init(iv);
    out_q_remote.Init(ov);

    double weight_sum = 0;

    for (auto v : iv) {
      auto oes = frag.GetOutgoingAdjList(v);
      for (auto& e : oes) {
        weight_sum += e.get_data();
      }
    }
    if (init_prio == 0) {
      /**
       * We select a similar heuristic, Δ = cw/d,
          where d is the average degree in the graph, w is the average
          edge weight, and c is the warp width (32 on our GPUs)
          Link: https://people.csail.mit.edu/jshun/papers/DBGO14.pdf
       */
      init_prio = 32 * (weight_sum / frag.GetEdgeNum()) /
                  (1.0 * frag.GetEdgeNum() / iv.size());
    }
    prio = init_prio;

    VLOG(1) << "In size: " << in_cap << " Local out size: " << local_out_cap
            << " Remote out size: " << remote_out_cap;

    messages.InitBuffer((sizeof(vid_t) + sizeof(dist_t)) * remote_out_cap,
                        (sizeof(vid_t) + sizeof(dist_t)) * in_cap);
    mm = &messages;
  }

  void Output(std::ostream& os) override {
    auto& frag = this->fragment();
    auto iv = frag.InnerVertices();

    dist.D2H();

    for (auto v : iv) {
      os << frag.GetId(v) << " " << dist[v] << std::endl;
    }
  }

  oid_t src_id;
  LoadBalancing lb{};
  VertexArray<dist_t, vid_t> dist;
  Queue<vertex_t> tmp_q;
  DenseVertexSet<vid_t> in_q, out_q_local_near, out_q_local_far;
  DenseVertexSet<vid_t> out_q_remote;
  dist_t init_prio{};
  dist_t prio{};
  double get_msg_time{};
  double traversal_kernel_time{};
  double send_msg_time{};
  GPUMessageManager* mm;
};

template <typename FRAG_T>
class SSSPOld : public GPUAppBase<FRAG_T, SSSPOldContext<FRAG_T>>,
                public ParallelEngine {
 public:
  INSTALL_GPU_WORKER(SSSPOld<FRAG_T>, SSSPOldContext<FRAG_T>, FRAG_T)
  using dist_t = typename context_t::dist_t;
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
                         dev::VertexArray<dist_t, vid_t> dist,
                         dev::DenseVertexSet<vid_t> in_q) {
            auto tid = TID_1D;

            if (tid == 0) {
              dist[source] = 0;
              in_q.Insert(source);
            }
          },
          frag.DeviceObject(), ctx.dist.DeviceObject(),
          ctx.in_q.DeviceObject());
    }
    messages.ForceContinue();
    messages.RecordUnpackTime(0);
    messages.RecordComputeTime(0);
    messages.RecordPackTime(0);
  }

  void IncEval(const fragment_t& frag, context_t& ctx,
               message_manager_t& messages) {
    auto d_dist = ctx.dist.DeviceObject();
    auto& tmp_q = ctx.tmp_q;
    auto d_tmp_q = tmp_q.DeviceObject();
    auto& in_q = ctx.in_q;
    auto d_in_q = in_q.DeviceObject();
    auto& out_q_local_near = ctx.out_q_local_near;
    auto& out_q_local_far = ctx.out_q_local_far;
    auto& out_q_remote = ctx.out_q_remote;
    auto d_out_q_remote = out_q_remote.DeviceObject();
    auto d_frag = frag.DeviceObject();
    auto& stream = messages.stream();
    auto d_mm = messages.DeviceObject();
    auto& prio = ctx.prio;
    auto iv = frag.InnerVertices();
    double t_unpack = 0, t_compute = 0, t_pack = 0;

    t_unpack -= grape::GetCurrentTime();
    messages.template ParallelProcess<dev_fragment_t, dist_t>(
        d_frag, [=] __device__(vertex_t v, dist_t received_dist) mutable {
          assert(d_frag.IsInnerVertex(v));

          if (received_dist < atomicMin(&d_dist[v], received_dist)) {
            d_in_q.Insert(v);
          }
        });
    stream.Sync();
    t_unpack += grape::GetCurrentTime();

    t_compute -= grape::GetCurrentTime();
    size_t in_size = in_q.Count(stream);

    out_q_remote.Clear(stream);

    if (in_size > 0) {
      auto d_in = in_q.DeviceObject();
      auto d_out_q_local_near = out_q_local_near.DeviceObject();
      auto d_out_q_local_far = out_q_local_far.DeviceObject();

      {
        WorkSourceRange<vertex_t> ws_in(iv.begin(), iv.size());

        tmp_q.Clear(stream);
        ForEach(stream, ws_in, [=] __device__(vertex_t v) mutable {
          if (d_in.Exist(v)) {
            d_tmp_q.AppendWarp(v);
          }
        });
      }

      WorkSourceArray<vertex_t> ws_in(tmp_q.data(), tmp_q.size(stream));

      stream.Sync();

      ForEachOutgoingEdge(
          stream, d_frag, ws_in,
          [=] __device__(vertex_t u) { return d_dist[u]; },
          [=] __device__(const VertexMetadata<vid_t, dist_t>& metadata,
                         const nbr_t& nbr) mutable {
            dist_t new_depth = metadata.metadata + nbr.get_data();
            vertex_t v = nbr.get_neighbor();
            if (new_depth < atomicMin(&d_dist[v], new_depth)) {
              if (d_frag.IsInnerVertex(v)) {
                if (new_depth < prio) {
                  d_out_q_local_near.Insert(v);
                } else {
                  d_out_q_local_far.Insert(v);
                }
              } else {
                d_out_q_remote.Insert(v);
              }
            }
          },
          ctx.lb);

      stream.Sync();
      in_q.Clear(stream);

      auto local_size = out_q_local_near.Count(stream);

      if (local_size > 0) {
        in_q.Swap(out_q_local_near);
      } else {
        local_size = out_q_local_far.Count(stream);
        in_q.Swap(out_q_local_far);
        prio += ctx.init_prio;
      }

      if (local_size > 0) {
        messages.ForceContinue();
      }
    }
    stream.Sync();
    t_compute += grape::GetCurrentTime();

    t_pack -= grape::GetCurrentTime();
    for (fid_t fid = 0; fid < frag.fnum(); fid++) {
      auto ov = frag.OuterVertices(fid);
      auto ws_in = WorkSourceRange<vertex_t>(ov.begin(), ov.size());

      ForEach(stream, ws_in, [=] __device__(vertex_t v) mutable {
        if (d_out_q_remote.Exist(v)) {
          d_mm.template SyncStateOnOuterVertexWarpOpt(d_frag, v, d_dist[v]);
        }
      });
    }
    stream.Sync();
    t_pack += grape::GetCurrentTime();

    messages.RecordUnpackTime(t_unpack);
    messages.RecordComputeTime(t_compute);
    messages.RecordPackTime(t_pack);
  }
};
}  // namespace grape_gpu

#endif  // GRAPEGPU_EXAMPLES_ANALYTICAL_APPS_SSSP_SSSP_OLD_H_