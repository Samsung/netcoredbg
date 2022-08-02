// Copyright (c) .NET Foundation. All rights reserved.
// Licensed under the Apache License, Version 2.0. See License.txt in the project root for license information.

using System;
using System.Threading.Tasks;

internal sealed class StartupHook
{
    internal static void ClearHotReloadEnvironmentVariables(
        Func<string, string?> getEnvironmentVariable,
        Action<string, string?> setEnvironmentVariable)
    {
        // Workaround for https://github.com/dotnet/runtime/issues/58000
        // Clear any hot-reload specific environment variables. This should prevent child processes from being
        // affected by the current app's hot reload settings.
        const string StartupHooksEnvironment = "DOTNET_STARTUP_HOOKS";
        var environment = getEnvironmentVariable(StartupHooksEnvironment);
        setEnvironmentVariable(StartupHooksEnvironment, RemoveCurrentAssembly(environment));

        static string? RemoveCurrentAssembly(string? environment)
        {
            if (string.IsNullOrEmpty(environment))
            {
                return environment;
            }

            var assemblyLocation = typeof(StartupHook).Assembly.Location;
            var updatedValues = environment.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries)
                .Where(e => !string.Equals(e, assemblyLocation, StringComparison.OrdinalIgnoreCase));

            return string.Join(Path.PathSeparator, updatedValues);
        }
    }

    public static void Initialize()
    {
        ClearHotReloadEnvironmentVariables(Environment.GetEnvironmentVariable, Environment.SetEnvironmentVariable);
        Task.Run((Action)ncdbloop);
    }

    public static void ncdbloop()
    {
        while (true)
        {
            ncdbfunc();
            System.Threading.Thread.Sleep(200);
        }
    }

    public static void ncdbfunc()
    {
    }
}
