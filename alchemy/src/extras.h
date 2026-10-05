/*
 * Preset extras: what no pot can express, carried in the preset blob so it
 * survives a power cycle -- the eight Morse words, the user pattern sent
 * from the web page, which source B1 picked, GATE / DUCK, the free tempo,
 * and the worst audio-block load of the last session.
 *
 * Every Alchemy firmware in the family has its own schema tag here ('STN'),
 * so a slot written by a sibling fails the schema gate and lands in this
 * firmware's first-boot defaults instead of being read as Stencil's.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include "alchemy/surface/serializable.h"

extern "C" {
#include "stencil.h"
}

struct StencilExtras : public alchemy::Serializable
{
    st_store_t store;
    uint8_t    source   = ST_SRC_BANK;
    uint8_t    polarity = ST_POL_GATE;
    float      free_bpm = 120.0f;
    float      cpu_peak = 0.0f;

    StencilExtras() { st_default_store(&store); }

    static constexpr size_t kWordBytes = ST_WORDS * (ST_WORD_MAX + 1u);
    static constexpr size_t kSize      = kWordBytes + 4u + 1u + 1u + 1u + 4u + 4u;

    size_t SerializedSize() const override { return kSize; }

    void Serialize(uint8_t* out) const override
    {
        std::memcpy(out, store.words, kWordBytes);
        out += kWordBytes;
        std::memcpy(out, &store.user_mask, 4u); out += 4;
        *out++ = store.user_len;
        *out++ = source;
        *out++ = polarity;
        std::memcpy(out, &free_bpm, 4u); out += 4;
        std::memcpy(out, &cpu_peak, 4u);
    }

    bool Deserialize(const uint8_t* in) override
    {
        st_store_t s;
        std::memcpy(s.words, in, kWordBytes);
        in += kWordBytes;
        std::memcpy(&s.user_mask, in, 4u); in += 4;
        s.user_len = *in++;
        uint8_t src = *in++, pol = *in++;
        float   b, pk;
        std::memcpy(&b, in, 4u); in += 4;
        std::memcpy(&pk, in, 4u);
        /* Garbage inside a valid blob: refuse it field by field. */
        for (unsigned w = 0; w < ST_WORDS; w++)
        {
            s.words[w][ST_WORD_MAX] = 0;
            for (unsigned i = 0; i < ST_WORD_MAX && s.words[w][i]; i++)
                if (s.words[w][i] < 0x20 || s.words[w][i] > 0x7E) { s.words[w][0] = 0; break; }
        }
        if (s.user_len < 1 || s.user_len > ST_MAX_STEPS) { s.user_len = 16; s.user_mask = 0x9249u; }
        if (src >= ST_SRC_COUNT) src = ST_SRC_BANK;
        if (pol > ST_POL_DUCK) pol = ST_POL_GATE;
        if (!(b > 20.0f && b < 300.0f)) b = 120.0f;
        if (!(pk >= 0.0f && pk <= 1.0f)) pk = 0.0f;
        store    = s;
        source   = src;
        polarity = pol;
        free_bpm = b;
        cpu_peak = pk;
        return true;
    }

    /* 'STN' + layout version. Bump the low byte when the layout changes. */
    uint32_t SchemaHash() const override { return 0x53544E00u | 0x01u; }
};
