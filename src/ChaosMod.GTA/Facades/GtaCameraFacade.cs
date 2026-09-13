using ChaosMod.Core.Abstractions;
using GTA.Native;

namespace ChaosMod.GTA.Facades
{
    public sealed class GtaCameraFacade : ICameraFacade
    {
        public void Shake(string shakeName, float amplitude) =>
            Function.Call(Hash.SHAKE_GAMEPLAY_CAM, shakeName, amplitude);

        public void StopShake() =>
            Function.Call(Hash.STOP_GAMEPLAY_CAM_SHAKING, true);

        public void SetFisheye(bool on)
        {
            // No first-class SHVDN wrapper for the fisheye lens effect; timecycle
            // modifiers are the standard approach. Verify the modifier name in-game.
            if (on)
                Function.Call(Hash.SET_TIMECYCLE_MODIFIER, "veryhighdof_fisheye");
            else
                Function.Call(Hash.CLEAR_TIMECYCLE_MODIFIER);
        }
    }
}
