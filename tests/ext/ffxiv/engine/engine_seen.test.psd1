@{
    Class       = 'cpu'
    Note        = 'Engine pass override shader index table (ext/ffxiv/engine/EnginePassRules.h): the slot count is a power of two and twice the index''s 40,960-bytecode cap, the hash stays inside the table, is stable for one address and spreads the addresses of one heap (16 KiB apart, differing only above the low bits) over thousands of slots, the linear probe puts colliding addresses on consecutive slots of one home and wraps the mask, a probe that runs out refuses the insert and leaves the stored addresses answering (fail open, the bound and not a full table), 5,000 synthetic shader addresses and the index''s whole 40,960-bytecode cap are stored and found with their own packed anchors, the bounded probe refusing at most the one address whose neighbourhood is taken, which is the module''s own full-table fail-open, a bind of an address the table does not hold asks the object no stamp at all while a hit asks once, an address whose object carries another layout''s id (or no stamp) is refused and re-indexing it replaces the record in place, a 24-byte record (3 MiB at 131,072 slots) that stores the whole 64-bit anchors word, an address re-indexed under another id refusing the previous one instead of answering the new anchors beside it, a layout with no anchor bit for any knob is not stored, a null address or a zero id is refused, and the full-table line says what happens'
    Sources     = @('engine_seen.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = @('c++20')
}
