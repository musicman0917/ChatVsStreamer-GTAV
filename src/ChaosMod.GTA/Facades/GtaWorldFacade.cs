using System;
using System.Numerics;
using ChaosMod.Core.Abstractions;
using GTA;
using GTA.Native;

namespace ChaosMod.GTA.Facades
{
    public sealed class GtaWorldFacade : IWorldFacade
    {
        public void SetWeather(string weatherType, float transitionSeconds)
        {
            if (!Enum.TryParse<Weather>(weatherType, true, out var weather))
            {
                Function.Call(Hash.SET_WEATHER_TYPE_NOW, weatherType);
                return;
            }

            if (transitionSeconds <= 0f)
                World.Weather = weather;
            else
                World.TransitionToWeather(weather, transitionSeconds);
        }

        public string GetCurrentWeather() => World.Weather.ToString();

        public void SetClockTime(int hour, int minute, int second) =>
            Function.Call(Hash.SET_CLOCK_TIME, hour, minute, second);

        public void AddExplosion(Vector3 position, int explosionType, float cameraShake, bool isAudible, bool isInvisibleDamage)
        {
            Function.Call(Hash.ADD_EXPLOSION,
                position.X, position.Y, position.Z,
                explosionType, 1.0f /* damageScale */, isAudible, isInvisibleDamage, cameraShake);
        }
    }
}
