@{
    Class       = 'warp'
    Note        = 'Contact shadow injection on D3D11 WARP: the pass copy is bound for exactly one draw and the game''s own shader resource is back afterwards (also when the draw throws), the cascade atlas at the mask slot is refused with the game''s binding untouched, a zero strength runs nothing, sixteen refusals switch the module off for the session with one line and nothing more is written after it, the log stays inside its budget and a shader the index never claimed is a pass-through that is not counted as a decline'
    Sources     = @('contact_apply_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/relight/ContactShadowsApply.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/relight/ContactShadowsPass.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/relight/ContactShadowsDump.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Prepare     = 'prepare_contact.ps1'
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
