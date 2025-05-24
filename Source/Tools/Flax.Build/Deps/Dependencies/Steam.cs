// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Linq;
using Flax.Build;
using Flax.Build.Platforms;

namespace Flax.Deps.Dependencies
{
    /// <summary>
    /// Windows Debug Help Library.
    /// </summary>
    /// <seealso cref="Flax.Deps.Dependency" />
    class Steam : Dependency
    {
        /// <inheritdoc />
        public override TargetPlatform[] Platforms
        {
            get
            {
                switch (BuildPlatform)
                {
                    case TargetPlatform.Windows:
                        return new[]
                        {
                        TargetPlatform.Windows,
                    };
                    default: return new TargetPlatform[0];
                }
            }
        }

        /// <inheritdoc />
        public override void Build(BuildOptions options)
        {
            foreach (var platform in options.Platforms)
            {
                BuildStarted(platform);
                switch (platform)
                {
                    case TargetPlatform.Windows:
                        {
                            var sdk = WindowsPlatformBase.GetSDKs().Last();
                            foreach (var architecture in new[] { TargetArchitecture.x64, TargetArchitecture.ARM64 })
                            {
                                var root = options.IntermediateFolder;
                                var depsFolder = GetThirdPartyFolder(options, platform, architecture);
                                var dllLocation = @$"{sdk.Value}Debuggers\lib\{architecture}\steam_api64.dll";
                                var appIdLocation = @$"{sdk.Value}Debuggers\{architecture}\steam_appid.txt";
                                foreach (var file in new[]
                                {
                                    "Steam/steam_api64.dll",
                                    "Steam/steam_appid.txt",
                                })
                                {
                                    Utilities.FileCopy(Path.Combine(root, file), Path.Combine(depsFolder, Path.GetFileName(file)));
                                }
                            }
                            break;
                        }
                    case TargetPlatform.Linux:
                        {
                            var root = options.IntermediateFolder;
                            var steamFolder = Path.Combine(root, "Steam");
                            var dllFileName = "steam_api64.dll";
                            var appIdFileName = "steam_appid.txt";
                            var depsFolder = GetThirdPartyFolder(options, platform, TargetArchitecture.x64);
                            Utilities.FileCopy(Path.Combine(steamFolder, dllFileName), Path.Combine(depsFolder, dllFileName));
                            Utilities.FileCopy(Path.Combine(steamFolder, appIdFileName), Path.Combine(depsFolder, appIdFileName));
                            break;
                        }
                }
            }
        }
    }
}
