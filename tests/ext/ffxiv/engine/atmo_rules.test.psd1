@{
    Class       = 'cpu'
    Note        = 'Atmosphere''s pure rules on the host: every key default and clamp (NaN reads as the default), Wanted, the optical depth of an exponential height medium (zero nearer than the start and with no medium, rho0 x length in a homogeneous medium and on a level ray, equal to a numeric integral looking up and down, monotone, series limit, capped), the transmittance (exactly 1 for no medium), the Henyey-Greenstein lobe (1 at the light, isotropic at g 0, forward above backward, monotone, exact reference value), the engine-fog scale and the night scale, the sun gain and the in-scatter (no sun runs on the ambient term), the surface rule (the far plane gets nothing), the view position through the capture''s inverse projection, the stored mix in linear light (T = 1 is bit-exact), the stand-down streak and the log lines, and (CAMFIX) the fold of the frame''s two camera copies (the sun draw''s first, the light draws'' as the stand-in, a refused table is no camera, only the sun draw''s copy refreshes the sun''s stamp) and the 180-frame ages of the camera and the sun, driven as frame sequences'
    Sources     = @('atmo_rules.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
