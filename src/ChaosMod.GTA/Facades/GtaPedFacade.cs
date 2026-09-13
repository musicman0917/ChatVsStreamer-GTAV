using System;
using System.Linq;
using ChaosMod.Core.Abstractions;
using GTA;
using GTA.Native;

namespace ChaosMod.GTA.Facades
{
    public sealed class GtaPedFacade : IPedFacade
    {
        private static readonly string[] HostilePedModels = { "g_m_y_lost_01", "a_m_y_gentransport", "s_m_y_marshall_01" };

        public void MakeNearbyPedsFlee(float radius)
        {
            var player = Game.Player.Character;
            foreach (var ped in World.GetNearbyPeds(player, radius))
            {
                if (ped == null || !ped.Exists() || ped == player) continue;
                ped.Task.ReactAndFlee(player);
            }
        }

        public void SpawnHostilePedsAround(int count, float radius)
        {
            var player = Game.Player.Character;
            var random = new Random();

            for (var i = 0; i < count; i++)
            {
                var angle = random.NextDouble() * Math.PI * 2;
                var offset = new GTA.Math.Vector3((float)Math.Cos(angle), (float)Math.Sin(angle), 0f) * radius;
                var spawnPos = player.Position + offset;

                var modelName = HostilePedModels[random.Next(HostilePedModels.Length)];
                var model = new Model(modelName);
                model.Request(1000);

                var ped = World.CreatePed(model, spawnPos);
                if (ped == null) continue;

                ped.Weapons.Give(WeaponHash.Pistol, 250, true, true);
                Function.Call(Hash.SET_RELATIONSHIP_BETWEEN_GROUPS, 5 /* hate */, ped.RelationshipGroup, player.RelationshipGroup);
                Function.Call(Hash.SET_RELATIONSHIP_BETWEEN_GROUPS, 5 /* hate */, player.RelationshipGroup, ped.RelationshipGroup);
                ped.Task.FightAgainst(player);
            }
        }
    }
}
