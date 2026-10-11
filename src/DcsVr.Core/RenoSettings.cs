namespace DcsVr.Core;

/// <summary>The advanced DLSS 5 tuning (the RenoDX DLSS 5 settings) back at their neutral defaults.</summary>
public static class RenoSettings
{
    public static VrProfile ResetAdvanced(VrProfile p) => p with
    {
        NeuralLocalTone = 1, NeuralLocalStructure = 1, NeuralSkinStructure = 1,
        NeuralAutomaticMask = false, NeuralUiCorrection = false,
        NeuralColorStrength = 1, NeuralTransferStrength = 1, NeuralPaperWhiteScale = 1,
        NeuralDepth = NeuralDepthMode.Game, NeuralMotionScaleX = 1, NeuralMotionScaleY = 1
    };
}
