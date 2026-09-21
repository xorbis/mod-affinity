/*
 * This file is part of mod-affinity, a module for AzerothCore, released under the
 * GNU GPL v2 license: https://github.com/xorbis/mod-affinity/blob/main/LICENSE
 */

void AddAffinityScripts();

// Called by the core's generated module loader; the name follows the folder name.
void Addmod_affinityScripts()
{
    AddAffinityScripts();
}
