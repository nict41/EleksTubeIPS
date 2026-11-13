// Native tests for the panel decoders: QoiStream (POST /panel?format=qoi and batches) and PanelBatch
// (POST /panel/batch records). Built and run by tools/native_tests.py, with AddressSanitizer and
// UndefinedBehaviorSanitizer, against QOI images Pillow made.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "QoiStream.h"
#include "PanelBatch.h"

static int failures = 0;
static int checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::vector<uint8_t> readFile(const std::string &path) {
	std::vector<uint8_t> out;
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) {
		return out;
	}
	uint8_t buf[4096];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
		out.insert(out.end(), buf, buf + n);
	}
	std::fclose(f);
	return out;
}

struct Pixels {
	std::vector<uint8_t> rgb;
	size_t limit;		// More pixels than this is a bug
	bool overflow = false;
};

static void collect(void *ctx, uint8_t r, uint8_t g, uint8_t b) {
	Pixels *p = (Pixels*)ctx;
	if (p->rgb.size() / 3 >= p->limit) {
		p->overflow = true;
		return;
	}
	p->rgb.push_back(r);
	p->rgb.push_back(g);
	p->rgb.push_back(b);
}

static uint32_t lcg = 12345;
static uint32_t rnd() {
	lcg = lcg * 1103515245u + 12345u;
	return lcg >> 8;
}

// Feeds data in chunks: size > 0 fixed, 0 = random 1..300. Returns the last result.
static QoiStream::Result decode(QoiStream &q, const std::vector<uint8_t> &data, size_t chunk, Pixels &px) {
	QoiStream::Result r = QoiStream::OK;
	size_t off = 0;
	while (off < data.size()) {
		size_t n = chunk ? chunk : 1 + rnd() % 300;
		if (n > data.size() - off) {
			n = data.size() - off;
		}
		r = q.feed(data.data() + off, n, collect, &px);
		off += n;
		if (r == QoiStream::ERROR) {
			break;
		}
	}
	return r;
}

static void testQoiFixtures(const std::string &dir) {
	FILE *m = std::fopen((dir + "/manifest.txt").c_str(), "r");
	CHECK(m != nullptr, "no manifest in %s", dir.c_str());
	if (!m) {
		return;
	}
	char name[128];
	unsigned w, h;
	int images = 0;
	while (std::fscanf(m, "%127s %u %u", name, &w, &h) == 3) {
		images++;
		std::vector<uint8_t> qoi = readFile(dir + "/" + name + ".qoi");
		std::vector<uint8_t> want = readFile(dir + "/" + name + ".rgb");
		CHECK(!qoi.empty() && want.size() == (size_t)w * h * 3, "%s: fixture missing", name);
		static QoiStream q;	// Static like on the clock (it's big for a stack)
		const size_t chunks[] = { 1, 2, 3, 7, 64, 4096, 0, 0, 0 };
		for (size_t c : chunks) {
			Pixels px;
			px.limit = (size_t)w * h;
			q.begin((uint16_t)w, (uint16_t)h);
			QoiStream::Result r = decode(q, qoi, c ? c : 0, px);
			CHECK(r == QoiStream::DONE, "%s chunk %zu: result %d (%s)", name, c, (int)r, q.error());
			CHECK(q.complete(), "%s chunk %zu: incomplete", name, c);
			CHECK(!px.overflow, "%s chunk %zu: more pixels than w*h", name, c);
			CHECK(px.rgb == want, "%s chunk %zu: pixels differ from Pillow's", name, c);
		}

		// Truncated: never complete, never an error, never too many pixels
		for (size_t cut : { (size_t)5, (size_t)14, qoi.size() / 2, qoi.size() - 9 }) {
			if (cut >= qoi.size()) {
				continue;
			}
			std::vector<uint8_t> part(qoi.begin(), qoi.begin() + cut);
			Pixels px;
			px.limit = (size_t)w * h;
			q.begin((uint16_t)w, (uint16_t)h);
			QoiStream::Result r = decode(q, part, 0, px);
			CHECK(r != QoiStream::ERROR, "%s cut %zu: error %s", name, cut, q.error());
			CHECK(!px.overflow, "%s cut %zu: too many pixels", name, cut);
		}

		// Flipped bytes after the header: anything may come out, but never past w*h (ASan checks memory)
		for (int t = 0; t < 40; t++) {
			std::vector<uint8_t> bad = qoi;
			for (int k = 0; k < 4; k++) {
				bad[14 + rnd() % (bad.size() - 14)] = (uint8_t)rnd();
			}
			Pixels px;
			px.limit = (size_t)w * h;
			q.begin((uint16_t)w, (uint16_t)h);
			decode(q, bad, 0, px);
			CHECK(!px.overflow, "%s flip %d: too many pixels", name, t);
		}

		// Wrong size asked for, and a bad magic
		{
			Pixels px;
			px.limit = (size_t)w * h + 1;
			q.begin((uint16_t)(w + 1), (uint16_t)h);
			CHECK(decode(q, qoi, 0, px) == QoiStream::ERROR, "%s: size mismatch not refused", name);
			CHECK(std::strcmp(q.error(), "QOI size doesn't match w x h") == 0, "%s: error %s", name, q.error());
			std::vector<uint8_t> bad = qoi;
			bad[0] = 'x';
			q.begin((uint16_t)w, (uint16_t)h);
			CHECK(decode(q, bad, 3, px) == QoiStream::ERROR, "%s: bad magic not refused", name);
			CHECK(std::strcmp(q.error(), "not a QOI file") == 0, "%s: error %s", name, q.error());
		}
	}
	std::fclose(m);
	CHECK(images >= 5, "only %d fixture images", images);
	std::printf("QOI: %d images\n", images);
}

// --- PanelBatch -----------------------------------------------------------------------------------

static void put16(std::vector<uint8_t> &b, unsigned v) {
	b.push_back((uint8_t)(v & 0xff));
	b.push_back((uint8_t)(v >> 8));
}

static void record(std::vector<uint8_t> &b, unsigned tube, unsigned fmt, unsigned x, unsigned y, unsigned w,
		unsigned h, uint32_t len, size_t actual = (size_t)-1) {
	b.push_back((uint8_t)tube);
	b.push_back((uint8_t)fmt);
	put16(b, x);
	put16(b, y);
	put16(b, w);
	put16(b, h);
	for (int i = 0; i < 4; i++) {
		b.push_back((uint8_t)(len >> (8 * i)));
	}
	size_t n = actual == (size_t)-1 ? len : actual;
	for (size_t i = 0; i < n; i++) {
		b.push_back((uint8_t)(i * 7));
	}
}

static const char *parse(const std::vector<uint8_t> &b, PanelBatch::Record *out, uint8_t max, uint8_t *count) {
	// A copy exactly as long as the body, so ASan catches any read past it
	uint8_t *copy = (uint8_t*)std::malloc(b.size() ? b.size() : 1);
	if (!b.empty()) {
		std::memcpy(copy, b.data(), b.size());
	}
	const char *err = PanelBatch::parse(copy, b.size(), out, max, count, 135, 240);
	if (err == nullptr && *count > 0) {
		// The data pointers must lie inside the copy
		for (uint8_t i = 0; i < *count; i++) {
			CHECK(out[i].data >= copy && out[i].data + out[i].len <= copy + b.size(), "record %u outside the body", i);
		}
	}
	std::free(copy);
	return err;
}

static void expectError(const std::vector<uint8_t> &b, const char *want, const char *what) {
	PanelBatch::Record recs[8];
	uint8_t count = 0;
	const char *err = parse(b, recs, 8, &count);
	CHECK(err != nullptr && std::strcmp(err, want) == 0, "%s: got '%s', want '%s'", what, err ? err : "ok", want);
}

static void testBatch() {
	PanelBatch::Record recs[8];
	uint8_t count = 0;

	std::vector<uint8_t> ok;
	record(ok, 0, 0, 0, 0, 135, 240, 135 * 240 * 2);	// whole tube, raw
	record(ok, 5, 1, 10, 20, 30, 40, 22);				// small QOI (length only is checked)
	record(ok, 3, 2, 0, 0, 135, 240, 4000);			// JPEG
	CHECK(parse(ok, recs, 8, &count) == nullptr, "valid batch refused");
	CHECK(count == 3, "count %u", count);
	CHECK(recs[0].tube == 0 && recs[0].format == 0 && PanelBatch::isWholeTube(recs[0], 135, 240), "record 0");
	CHECK(recs[1].tube == 5 && recs[1].x == 10 && recs[1].y == 20 && recs[1].w == 30 && recs[1].h == 40
		&& recs[1].len == 22 && !PanelBatch::isWholeTube(recs[1], 135, 240), "record 1");
	CHECK(recs[2].format == 2 && recs[2].len == 4000, "record 2");

	std::vector<uint8_t> b;
	expectError(b, "a batch needs at least one record", "empty");
	b.assign(13, 0);
	expectError(b, "batch ends part way through a record", "short header");
	b.clear(); record(b, 6, 0, 0, 0, 1, 1, 2);
	expectError(b, "batch record: tube must be 0 to 5", "tube 6");
	b.clear(); record(b, 0, 0, 0, 0, 0, 10, 0);
	expectError(b, "batch record: rectangle must be inside the tube", "w 0");
	b.clear(); record(b, 0, 0, 100, 0, 36, 10, 36 * 10 * 2);
	expectError(b, "batch record: rectangle must be inside the tube", "past the right edge");
	b.clear(); record(b, 0, 0, 0, 0xFFFF, 10, 10, 200);
	expectError(b, "batch record: rectangle must be inside the tube", "negative y");
	b.clear(); record(b, 0, 0, 0, 0, 136, 10, 2720);
	expectError(b, "batch record: rectangle must be inside the tube", "too wide");
	b.clear(); record(b, 0, 0, 0, 0, 10, 10, 199);
	expectError(b, "batch record: rgb565 length must be w*h*2", "raw length");
	b.clear(); record(b, 0, 1, 0, 0, 10, 10, 21);
	expectError(b, "batch record: not a QOI image", "tiny QOI");
	b.clear(); record(b, 0, 2, 0, 0, 10, 10, 3);
	expectError(b, "batch record: not a JPEG", "tiny JPEG");
	b.clear(); record(b, 0, 3, 0, 0, 10, 10, 30);
	expectError(b, "batch record: format must be 0 (rgb565), 1 (qoi) or 2 (jpeg)", "format 3");
	b.clear(); record(b, 0, 0xFF, 0, 0, 10, 10, 30);
	expectError(b, "batch record: format must be 0 (rgb565), 1 (qoi) or 2 (jpeg)", "the internal pattern format");
	b.clear(); record(b, 0, 1, 0, 0, 10, 10, 0xFFFFFFFFu, 30);
	expectError(b, "batch ends part way through a record", "length past the end");
	b.clear(); record(b, 0, 1, 0, 0, 10, 10, 100, 60);
	expectError(b, "batch ends part way through a record", "cut picture");
	b.clear(); record(b, 0, 0, 0, 0, 1, 1, 2); b.push_back(0);
	expectError(b, "batch ends part way through a record", "a stray byte");
	b.clear();
	for (int i = 0; i < 9; i++) {
		record(b, (unsigned)(i % 6), 0, 0, 0, 1, 1, 2);
	}
	expectError(b, "too many records in one batch", "9 records, room for 8");

	// Random garbage never reads outside the body (ASan) and never crashes
	for (int t = 0; t < 2000; t++) {
		std::vector<uint8_t> g(rnd() % 120);
		for (auto &x : g) {
			x = (uint8_t)rnd();
		}
		if (g.size() >= 14 && t % 2) {
			g[0] %= 6; g[1] %= 3; g[7] = 0; g[9] = 0; g[11] = 0; g[12] = 0; g[13] = 0;	// often plausible headers
		}
		parse(g, recs, 8, &count);
	}
	std::printf("PanelBatch: ok\n");
}

int main(int argc, char **argv) {
	if (argc < 2) {
		std::printf("usage: %s <fixture dir>\n", argv[0]);
		return 2;
	}
	testQoiFixtures(argv[1]);
	testBatch();
	std::printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
