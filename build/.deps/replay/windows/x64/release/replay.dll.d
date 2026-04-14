{
    files = {
        [[build\.objs\replay\windows\x64\release\src\mod\MemoryOperators.cpp.obj]],
        [[build\.objs\replay\windows\x64\release\src\mod\MyMod.cpp.obj]],
        [[build\.objs\replay\windows\x64\release\src\replay\Record\Recorder.cpp.obj]],
        [[build\.objs\replay\windows\x64\release\src\replay\playback\ReplayReader.cpp.obj]]
    },
    values = {
        [[D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\HostX64\x64\link.exe]],
        {
            "-nologo",
            "-machine:x64",
            [[-libpath:build\.prelink\lib]],
            [[-libpath:C:\Users\32204\AppData\Local\.xmake\packages\l\levilamina\26.10.6\4973ab13ed864f4e813d1acd294661c5\lib]],
            [[-libpath:C:\Users\32204\AppData\Local\.xmake\packages\f\fmt\11.2.0\ee1902dd7cff4195a287949f80dbce70\lib]],
            [[-libpath:C:\Users\32204\AppData\Local\.xmake\packages\l\leveldb\1.23\2532d000dc9d47cbb618c80ba0e6d806\lib]],
            [[-libpath:C:\Users\32204\AppData\Local\.xmake\packages\s\snappy\1.2.2\b3244272357b4994abb43dc0aa939d28\lib]],
            [[-libpath:C:\Users\32204\AppData\Local\.xmake\packages\s\symbolprovider\v1.2.0\716edebcb0f14dbbbc97a0d6aa352593\lib]],
            "/opt:ref",
            "/opt:icf",
            "-debug",
            [[-pdb:build\windows\x64\release\replay.pdb]],
            "bedrock_runtime_api.lib",
            "LeviLamina.lib",
            "fmt.lib",
            "leveldb.lib",
            "snappy.lib",
            "SymbolProvider.lib",
            "/DELAYLOAD:bedrock_runtime.dll"
        }
    }
}