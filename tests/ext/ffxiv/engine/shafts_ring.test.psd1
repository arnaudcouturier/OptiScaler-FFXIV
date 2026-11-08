@{
    Class       = 'warp'
    Note        = 'The sun''s light and camera read-back (ext/ffxiv/engine/ShaftsSun.cpp) on D3D11 WARP with real buffers: the first frame of a session reads nothing at all, every read that lands holds the values of the newest copy the ring still holds (the frame before this one) rather than the frame that just copied them, a copy this frame made is never read, a newer copy is never replaced by an older one, an older copy is taken when the newest has not landed, a light at a world position and a camera buffer that is not a projection are reported, a not-ready map is counted apart from a read, the projection shape check accepts the tolerance a live buffer needs and refuses a wrong row or a NaN, and Reset drops the facts and the staging pairs, and the directional shadow read with the light: the light draw''s 128-byte candidates are copied into the same ring slot, the shaped one carries the light''s own axis at +0x20, a neighbour without the matrix''s shape is kept but never judged, a draw with none is Missing rather than a refusal, and the rows belong to the copy the read took; the tone map''s ring is four deep, so a copy two frames back still answers a GPU that is behind, and a copy a read took is not handed out twice'
    Sources     = @('shafts_ring.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/ShaftsSun.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
