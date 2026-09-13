using System.Numerics;
using ChaosMod.Core.Abstractions;
using GTA;
using GTA.Native;

namespace ChaosMod.GTA.Facades
{
    public sealed class GtaPlayerFacade : IPlayerFacade
    {
        public bool IsInInterior => Function.Call<bool>(Hash.IS_ENTITY_IN_ANY_ROOM, Game.Player.Character);
        public bool IsInVehicle => Game.Player.Character.IsInVehicle();
        public int Health => Game.Player.Character.Health;
        public int MaxHealth => Game.Player.Character.MaxHealth;
        public Vector3 Position
        {
            get
            {
                var p = Game.Player.Character.Position;
                return new Vector3(p.X, p.Y, p.Z);
            }
        }

        public void SetHealth(int value) => Game.Player.Character.Health = value;
        public void SetArmor(int value) => Game.Player.Character.Armor = value;

        public void SetWantedLevel(int stars, bool applyNow)
        {
            Function.Call(Hash.SET_PLAYER_WANTED_LEVEL, Game.Player, stars, false);
            if (applyNow) Function.Call(Hash.SET_PLAYER_WANTED_LEVEL_NOW, Game.Player, false);
        }

        public void ClearWantedLevel() => Function.Call(Hash.CLEAR_PLAYER_WANTED_LEVEL, Game.Player);

        public void GiveWeapon(uint weaponHash, int ammo, bool equipNow)
        {
            Game.Player.Character.Weapons.Give((WeaponHash)weaponHash, ammo, equipNow, true);
        }

        public void SetCurrentWeaponAmmo(int ammo)
        {
            var current = Game.Player.Character.Weapons.Current;
            if (current != null) current.Ammo = ammo;
        }

        public uint GetCurrentWeaponHash()
        {
            var current = Game.Player.Character.Weapons.Current;
            return current != null ? (uint)current.Hash : 0u;
        }

        public void SetOnFire() => Function.Call(Hash.START_ENTITY_FIRE, Game.Player.Character);

        public void Ragdoll(int minMs, int maxMs) =>
            Function.Call(Hash.SET_PED_TO_RAGDOLL, Game.Player.Character, minMs, maxMs, 0, true, true, false);

        public void SetSuperJump(bool on) =>
            Function.Call(Hash.SET_SUPER_JUMP_THIS_FRAME, Game.Player); // must be re-applied every frame by the caller while active

        public void SetInfiniteAmmo(bool on) =>
            Function.Call(Hash.SET_PLAYER_INFINITE_AMMO_CLIP, Game.Player, on);

        // SET_GRAVITY_LEVEL takes an index (0 = normal, 1 = moon, 2 = none) rather than a
        // continuous scale; verify these indices in-game if low-gravity feels off.
        public void SetGravityScale(float scale) => Function.Call(Hash.SET_GRAVITY_LEVEL, scale <= 0.2f ? 1 : 0);

        public void ApplyControlInversion(bool on)
        {
            // Re-applied every frame while active by the owning effect's Tick(): disables the
            // normal movement/look axes and re-issues them negated via SET_CONTROL_NORMAL.
            if (!on) return;

            DisableAndInvert(Control.MoveLeftRight);
            DisableAndInvert(Control.MoveUpDown);
            DisableAndInvert(Control.LookLeftRight);
            DisableAndInvert(Control.LookUpDown);
        }

        private static void DisableAndInvert(Control control)
        {
            var value = Function.Call<float>(Hash.GET_DISABLED_CONTROL_NORMAL, 0, (int)control);
            Function.Call(Hash.DISABLE_CONTROL_ACTION, 0, (int)control, true);
            Function.Call(Hash.SET_CONTROL_NORMAL, 0, (int)control, -value);
        }

        public void SetHudVisible(bool visible) => Function.Call(Hash.DISPLAY_HUD, visible);
        public void SetRadarVisible(bool visible) => Function.Call(Hash.DISPLAY_RADAR, visible);
    }
}
