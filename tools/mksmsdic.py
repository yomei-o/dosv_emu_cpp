"""Build 鳳's two dictionaries out of an SKK dictionary.

    python tools/mksmsdic.py fep/SKK-JISYO.L out/

「鳳」(otri.sys) will not install without `smsrea.dic` and `smsdic.dic`. They are
normally converted from 「風」's own dictionary with the `convdic.exe` that comes
with it -- but 風's dictionary is not something you can still download, and the
format is written down twice over: in DICTOOL.DOC and in CONVDIC.C, which comes
with the converter as source. So the dictionary can be built from any word list,
and the one to hand is SKK's.

The format, from CONVDIC.C:

  smsdic.dic   three bytes per kanji, in reading order:
                 byte 0  the JIS code's first byte, with bit 7 holding bit 8
                         of the key position
                 byte 1  the JIS code's second byte
                 byte 2  the key position's low eight bits
  smsrea.dic   eight words, the address of the *last* reading of each length
               1..8; then the readings themselves, grouped by length, each one
                 reading (length bytes, half-width katakana, one byte a kana)
                 + pointer into smsdic.dic, 15 bits, counted in entries
                 + number of kanji, 9 bits
               packed as: [len] = pointer low, [len+1] = pointer high 7 bits |
               (count bit 8) << 7, [len+2] = count low.

「鳳」 is a single-kanji FEP: one reading offers a keyboard of kanji and you pick
one, so only single-kanji candidates are any use to it. The key position is what
decides where a candidate sits on that keyboard, and with nothing better to say,
the order the dictionary lists them in is the order they appear.
"""
import os
import sys

# JIS X 0201 half-width katakana, which is what a reading is written in: one
# byte a kana, with the voiced marks as separate bytes.
KANA = {}
for i, ch in enumerate('ｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜｦﾝ'):
    KANA[ch] = i
FULL = 'アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲン'
HIRA = 'あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをん'
BASE = {}
for h, f, k in zip(HIRA, FULL, 'ｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜｦﾝ'):
    BASE[h] = k.encode('cp932')
VOICED = {'が': 'ｶﾞ', 'ぎ': 'ｷﾞ', 'ぐ': 'ｸﾞ', 'げ': 'ｹﾞ', 'ご': 'ｺﾞ',
          'ざ': 'ｻﾞ', 'じ': 'ｼﾞ', 'ず': 'ｽﾞ', 'ぜ': 'ｾﾞ', 'ぞ': 'ｿﾞ',
          'だ': 'ﾀﾞ', 'ぢ': 'ﾁﾞ', 'づ': 'ﾂﾞ', 'で': 'ﾃﾞ', 'ど': 'ﾄﾞ',
          'ば': 'ﾊﾞ', 'び': 'ﾋﾞ', 'ぶ': 'ﾌﾞ', 'べ': 'ﾍﾞ', 'ぼ': 'ﾎﾞ',
          'ぱ': 'ﾊﾟ', 'ぴ': 'ﾋﾟ', 'ぷ': 'ﾌﾟ', 'ぺ': 'ﾍﾟ', 'ぽ': 'ﾎﾟ',
          'ぁ': 'ｧ', 'ぃ': 'ｨ', 'ぅ': 'ｩ', 'ぇ': 'ｪ', 'ぉ': 'ｫ',
          'ゃ': 'ｬ', 'ゅ': 'ｭ', 'ょ': 'ｮ', 'っ': 'ｯ', 'ー': 'ｰ'}
for h, s in VOICED.items():
    BASE[h] = s.encode('cp932')


def reading_bytes(kana):
    """A hiragana reading as 鳳 stores it, or None if it cannot be."""
    out = bytearray()
    for ch in kana:
        b = BASE.get(ch)
        if b is None:
            return None
        out += b
    return bytes(out) if 1 <= len(out) <= 8 else None


def sjis_to_jis(code):
    """Shift-JIS to the two seven-bit JIS bytes the dictionary holds."""
    c1, c2 = code >> 8, code & 0xFF
    if not (0x81 <= c1 <= 0x9F or 0xE0 <= c1 <= 0xEF):
        return None
    row = (c1 - (0x81 if c1 < 0xA0 else 0xC1)) * 2 + 1
    if c2 >= 0x9F:
        row += 1
        cell = c2 - 0x9E
    else:
        cell = c2 - (0x3F if c2 < 0x7F else 0x40)
    if not (1 <= row <= 94 and 1 <= cell <= 94):
        return None
    return ((row + 0x20) << 8) | (cell + 0x20)


def main(skk, outdir):
    readings = {}                                   # reading bytes -> [jis codes]
    plain = False
    for line in open(skk, 'rb'):
        if line[:1] == b';':
            if b'okuri-nasi' in line:
                plain = True
            continue
        if not plain:
            continue
        try:
            text = line.decode('euc_jp').rstrip('\n')
        except UnicodeDecodeError:
            continue
        sp = text.find(' ')
        if sp <= 0:
            continue
        key = reading_bytes(text[:sp])
        if not key:
            continue
        got = []
        for cand in text[sp + 1:].split('/'):
            cand = cand.split(';')[0]
            if len(cand) != 1:                      # one kanji is all it can show
                continue
            try:
                sj = cand.encode('cp932')
            except UnicodeEncodeError:
                continue
            if len(sj) != 2:
                continue
            jis = sjis_to_jis((sj[0] << 8) | sj[1])
            if jis and jis not in got:
                got.append(jis)
        if got:
            readings.setdefault(key, [])
            for j in got:
                if j not in readings[key]:
                    readings[key].append(j)

    # The pointer into smsdic.dic is fifteen bits, counted in entries, so the
    # whole dictionary has to fit in 32,767 kanji. Short readings first: they
    # are the ones a single-kanji FEP is for.
    order = sorted(readings, key=lambda k: (len(k), k))
    dic = bytearray()
    rea = {n: bytearray() for n in range(1, 9)}
    at = 0
    kept = 0
    for key in order:
        codes = readings[key][:320]
        if at + len(codes) > 32767:
            break
        for pos, jis in enumerate(codes):
            b0 = (jis >> 8) | ((pos & 0x100) >> 1)
            dic += bytes((b0, jis & 0xFF, pos & 0xFF))
        n = len(codes)
        rea[len(key)] += key + bytes(((at & 0xFF),
                                      ((at >> 8) | ((n & 0x100) >> 1)),
                                      n & 0xFF))
        at += n
        kept += 1

    out = bytearray(16)
    adr = 16
    for length in range(1, 9):
        body = rea[length]
        out += body
        adr += len(body)
        last = adr - (3 + length) if body else adr
        out[(length - 1) * 2] = last & 0xFF
        out[(length - 1) * 2 + 1] = (last >> 8) & 0xFF

    os.makedirs(outdir, exist_ok=True)
    open(os.path.join(outdir, 'SMSREA.DIC'), 'wb').write(bytes(out))
    open(os.path.join(outdir, 'SMSDIC.DIC'), 'wb').write(bytes(dic))
    print('%d readings, %d kanji -> SMSREA.DIC %d bytes, SMSDIC.DIC %d bytes'
          % (kept, at, len(out), len(dic)))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else '.')
