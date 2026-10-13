/* Chrome WebCodecs -> timestamped VP8 WebM, no external encoder/muxer.
 * Each source frame keeps its CDP timestamp relative to the first frame.
 * Matroska elements: https://www.matroska.org/technical/elements.html
 */
(() => {
    const concat = parts => {
        const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
        let at = 0;
        for (const p of parts) { out.set(p, at); at += p.length; }
        return out;
    };
    const hex = s => Uint8Array.from(s.match(/../g).map(x => parseInt(x, 16)));
    const uint = n => {
        const bytes = [];
        do { bytes.unshift(n % 256); n = Math.floor(n / 256); } while (n);
        return Uint8Array.from(bytes);
    };
    const size = n => {
        for (let len = 1; len <= 8; len++) {
            if (n < 2 ** (7 * len) - 1) {
                const out = new Uint8Array(len);
                for (let i = len - 1; i >= 0; i--) { out[i] = n % 256; n = Math.floor(n / 256); }
                out[0] |= 1 << (8 - len);
                return out;
            }
        }
        throw new Error('WebM element too large');
    };
    const el = (id, data) => concat([hex(id), size(data.length), data]);
    const integer = (id, n) => el(id, uint(n));
    const str = (id, s) => el(id, new TextEncoder().encode(s));
    let encoder, chunks = [], error;
    const flush = () => new Promise((resolve,reject) => {
        const timer=setTimeout(()=>reject(new Error('VP8 flush timed out')),15000);
        encoder.flush().then(()=>{clearTimeout(timer);resolve();}, e=>{clearTimeout(timer);reject(e);});
    });
    window.demoVideoStart = async (width, height) => {
        const config = { codec: 'vp8', width, height, bitrate: 3500000, framerate: 5 };
        if (!(await VideoEncoder.isConfigSupported(config)).supported) throw new Error('VP8 encoder unavailable');
        encoder = new VideoEncoder({
            output(chunk) {
                const bytes = new Uint8Array(chunk.byteLength); chunk.copyTo(bytes);
                chunks.push({ timestamp: chunk.timestamp, key: chunk.type === 'key', bytes });
            }, error(e) { error = String(e); },
        });
        encoder.configure(config);
    };
    window.demoVideoFrame = async (base64, timestamp, key) => {
        if (error) throw new Error(error);
        const bytes = Uint8Array.from(atob(base64), c => c.charCodeAt(0));
        const bitmap = await createImageBitmap(new Blob([bytes], {type: 'image/jpeg'}));
        const frame = new VideoFrame(bitmap, {timestamp});
        encoder.encode(frame, {keyFrame: key}); frame.close(); bitmap.close();
        if (encoder.encodeQueueSize > 8) await flush();
    };
    window.demoVideoFinish = async (width, height, durationMs) => {
        await flush(); encoder.close();
        if (error || chunks.length < 2) throw new Error(error || 'no encoded video frames');
        const float = new Uint8Array(8); new DataView(float.buffer).setFloat64(0, durationMs);
        const header = el('1a45dfa3', concat([integer('4286', 1), integer('42f7', 1),
            integer('42f2', 4), integer('42f3', 8), str('4282', 'webm'),
            integer('4287', 2), integer('4285', 2)]));
        const info = el('1549a966', concat([integer('2ad7b1', 1000000),
            str('4d80', 'demo-walk-recorder'), str('5741', 'Chrome WebCodecs'), el('4489', float)]));
        const tracks = el('1654ae6b', el('ae', concat([integer('d7', 1), integer('73c5', 1),
            integer('83', 1), str('86', 'V_VP8'),
            el('e0', concat([integer('b0', width), integer('ba', height)]))])));
        const clusters = chunks.map(c => el('1f43b675', concat([
            integer('e7', Math.round(c.timestamp/1000)),
            el('a3', concat([Uint8Array.of(0x81, 0, 0, c.key ? 0x80 : 0), c.bytes])),
        ])));
        // Cues and their SeekHead make the long walk seekable in Chrome.
        // SeekPosition uses fixed eight-byte integers so header size is stable.
        const seekHead = position => {
            const bytes=new Uint8Array(8);
            for(let i=7;i>=0;i--){bytes[i]=position%256;position=Math.floor(position/256);}
            return el('114d9b74',el('4dbb',concat([el('53ab',hex('1c53bb6b')),el('53ac',bytes)])));
        };
        const prefixSize=seekHead(0).length+info.length+tracks.length;
        let position=prefixSize;
        const points=[];
        chunks.forEach((c,i)=>{
            if(c.key) points.push(el('bb',concat([
                integer('b3',Math.round(c.timestamp/1000)),
                el('b7',concat([integer('f7',1),integer('f1',position)])),
            ])));
            position+=clusters[i].length;
        });
        const cues=el('1c53bb6b',concat(points));
        const data = concat([header, el('18538067', concat([seekHead(position),info, tracks, ...clusters,cues]))]);
        window.demoEncodedVideo = data;
        return { bytes: data.length, frames: chunks.length };
    };
    window.demoVideoBytes = (start, end) => {
        let str = '';
        for (const byte of window.demoEncodedVideo.subarray(start, end)) str += String.fromCharCode(byte);
        return btoa(str);
    };
})();
