# SMAA Plugin
SMAA plugin for Unreal Engine 5.4+, copied from code from this PR: [Enhanced Subpixel Morphological Anti-Aliasing for UE5](https://github.com/EpicGames/UnrealEngine/pull/11840/)

The plugin is currently nonfunctional right now, there are issues with ghosting around edges that need to be fixed. If you have written an SMAA pass before and can help debug the issues with the shader, please feel free to put in a PR or reach out to me!

## How to Use 
Turn off antialiasing with the command `r.AntiAliasingMethod 0` and then enable SMAA with the command `r.SMAA 1`

[Demonstration video](https://www.youtube.com/watch?v=UT8kHgAnibU)
