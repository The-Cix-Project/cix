/*
 * Reading a PBS recipe's identity out of `cbs explain --json`
 * (ADR-0305).
 *
 * The fixtures are the REAL output shape, transcribed from CBS's own
 * emitter (its src/main.c explain_file()) at v0.1.24 rather than
 * imagined: keys in that order, `release` a bare number, `sources` an
 * array of {name, urls[], sha256}, `requires` an object of role
 * objects of keyword arrays, and null -- not an absent key -- for an
 * undeclared upstream or toolchain.
 *
 * `capabilities` appears in BOTH shapes here on purpose. It was a
 * COUNT until cix-build-system#162 and is an array of names from
 * cix-build-system main at 170dc744d onward. cixd execs whichever cbs
 * is installed on the host, so both shapes are documents it can really
 * be handed, and the count shape must be REFUSED rather than read as
 * zero -- a downgrade that silently dropped a declared CAP_SYS_ADMIN
 * would be the defect ADR-0304 was written about. The array fixture is
 * transcribed from that tree's own cli-contract-test.sh, which asserts
 * `"capabilities":["CAP_ONE","CAP_TWO"]`.
 *
 * This test exists because the daemon's own package test cannot run on
 * a Cix host at all: test_pkg needs the ADR-0209 floor artifacts, a
 * hand-fetched input that is deliberately not in the source tarball
 * (#485), and it is not in SELFTESTS either. A mapping written inside
 * pkg.c would therefore be gated by nothing. Everything here is pure
 * -- literal JSON in, answers out, no filesystem and no child process
 * -- so it runs in a build container and is a real gate.
 */
#include "pbsrecipe.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void fail(const char *what)
{
	fprintf(stderr, "  FAIL: %s\n", what);
	g_failures++;
}

static void expect_str(const char *what, const char *got, const char *want)
{
	if (got == NULL || strcmp(got, want) != 0) {
		fprintf(stderr, "  FAIL: %s: got \"%s\", want \"%s\"\n", what,
		        got == NULL ? "(null)" : got, want);
		g_failures++;
	}
}

static void expect_int(const char *what, int got, int want)
{
	if (got != want) {
		fprintf(stderr, "  FAIL: %s: got %d, want %d\n", what, got, want);
		g_failures++;
	}
}

/* zstd.cbs's real shape, as CBS would explain it. */
static const char *const ZSTD_JSON =
    "{\"name\":\"zstd\",\"version\":\"1.5.4\",\"release\":1,\"architecture\":null,"
    "\"sources\":[{\"name\":\"zstd\",\"urls\":[\"https://github.com/facebook/zstd/archive/"
    "refs/tags/v1.5.4.tar.gz\"],\"sha256\":\"35ad983197f8f8eb0c963877bf8be50490a0b3df54b4"
    "edeb8399ba8a8b2f60a4\"}],"
    "\"requires\":{\"build\":{\"compiler\":[\"tcc\"],\"tool\":[\"make\",\"bash\","
    "\"coreutils\",\"binutils\"]}},"
    "\"build_image\":null,\"upstream\":null,\"toolchain\":null,\"toolchain_reason\":null,"
    "\"capabilities\":[],\"phases\":[{\"name\":\"prepare\",\"operations\":4},"
    "{\"name\":\"build\",\"operations\":1},{\"name\":\"check\",\"operations\":1},"
    "{\"name\":\"install\",\"operations\":1}]}";

static void test_happy_path(void)
{
	char err[256] = {0};
	struct pbs_explain *ex = pbs_explain_parse(ZSTD_JSON, strlen(ZSTD_JSON), err, sizeof(err));
	char buf[512];
	char url[1024];
	char sha[128];

	if (ex == NULL) {
		fail("a real explain document failed to parse");
		fprintf(stderr, "        err: %s\n", err);
		return;
	}

	expect_str("name", pbs_explain_name(ex), "zstd");

	/*
	 * ADR-0307 clause 7: this fixture is a document as a cbs older
	 * than v0.1.26 wrote one -- no `format` key at all. It must read
	 * as "" rather than crash or invent a value, because "" is what
	 * the daemon keys the re-derivation refusal on. A fixture that
	 * carried the key would test the easy half only.
	 */
	expect_str("format absent in an old document", pbs_explain_format(ex), "");

	/*
	 * The fused version, and the whole reason this is worth asserting
	 * rather than trusting: cixd has ONE version string and CPDL has
	 * two fields. A join that silently dropped the release would
	 * publish 1.5.4 over the top of a different revision's directory,
	 * and ADR-0107 immutability would then refuse the real one.
	 */
	if (pbs_explain_version(ex, buf, sizeof(buf)) != 0)
		fail("version fusion failed");
	else
		expect_str("fused version", buf, "1.5.4-1");

	expect_int("source count", pbs_explain_source_count(ex), 1);
	if (pbs_explain_source(ex, 0, url, sizeof(url), sha, sizeof(sha)) != 0) {
		fail("source 0 could not be read");
	} else {
		expect_str("source url", url,
		           "https://github.com/facebook/zstd/archive/refs/tags/v1.5.4.tar.gz");
		expect_str("source sha256", sha,
		           "35ad983197f8f8eb0c963877bf8be50490a0b3df54b4edeb8399ba8a8b2f60a4");
	}

	/* Build tools: compiler and tool are separate CPDL keywords and
	 * both are pkg_build_depends to cixd, so each is asked for
	 * separately and the caller joins them. */
	if (pbs_explain_requires(ex, "build", "compiler", buf, sizeof(buf)) != 0)
		fail("requires build/compiler failed");
	else
		expect_str("build compiler", buf, "tcc");
	if (pbs_explain_requires(ex, "build", "tool", buf, sizeof(buf)) != 0)
		fail("requires build/tool failed");
	else
		expect_str("build tools", buf, "make bash coreutils binutils");

	/*
	 * An absent role is not an error. zstd declares no runtime
	 * dependencies, and a package whose runtime closure is libc is
	 * ordinary -- declared_provided_sonames() adds libc itself.
	 */
	if (pbs_explain_requires(ex, "runtime", "package", buf, sizeof(buf)) != 0)
		fail("an absent runtime role was reported as an error");
	else
		expect_str("absent runtime role", buf, "");

	/* null, not absent: str_or_empty() has to answer "" for both. */
	expect_str("upstream when null", pbs_explain_upstream(ex), "");
	expect_str("toolchain when null", pbs_explain_toolchain(ex), "");

	if (pbs_explain_capabilities(ex, buf, sizeof(buf)) != 0)
		fail("an empty capability array should read as none, not as an error");
	else
		expect_str("no capabilities declared", buf, "");

	/* No metadata block at all is "absent", not an error -- exactly as
	 * a shell recipe with no pkg_artifact_sha256= line parses fine. */
	if (pbs_explain_metadata(ex, "artifact_sha256", buf, sizeof(buf)) != 0)
		fail("an absent metadata block should not be an error");
	else
		expect_str("absent artifact_sha256", buf, "");

	expect_int("phase count", pbs_explain_phase_count(ex), 4);

	pbs_explain_free(ex);
}

static void test_release_and_runtime(void)
{
	/* The kernel's shape: a release that is NOT 1, which is the case
	 * that proves the fusion carries it. Plus a runtime package list,
	 * which is the free-form `package` keyword cix-build-system#160
	 * asks upstream to bless -- CPDL puts no allow-list on an item
	 * keyword, so this already validates and reaches an embedder. */
	static const char *const json =
	    "{\"name\":\"kernel\",\"version\":\"7.2.3\",\"release\":15,\"architecture\":null,"
	    "\"sources\":[{\"name\":\"linux\",\"urls\":[\"https://cdn.kernel.org/x.tar.xz\"],"
	    "\"sha256\":\"8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03\"},"
	    "{\"name\":\"config\",\"urls\":[\"https://git.home.arpa/raw/qemu-part1.config\"],"
	    "\"sha256\":\"a3fda92c6313292c48a4ad98728772aa156ea234bd048ae49811f6f5c171bc46\"}],"
	    "\"requires\":{\"build\":{\"compiler\":[\"gcc\"],\"tool\":[\"bash\",\"bc\"]},"
	    "\"runtime\":{\"package\":[\"openssl\",\"libarchive\",\"curl\"]}},"
	    "\"build_image\":null,\"upstream\":\"kernel.org\",\"toolchain\":\"gcc\","
	    "\"toolchain_reason\":\"the kernel does not build with TCC\","
	    "\"license\":\"GPL-2.0-only\","
	    "\"metadata\":{\"artifact_sha256\":\"c09de99506f24a17786da6507775c2fb3e5e3882"
	    "ec25e234f85632ec5f4607a8\",\"changelog\":\"7.2.3-15: real revision note\"},"
	    "\"capabilities\":[\"CAP_SYS_ADMIN\"],"
	    "\"phases\":[{\"name\":\"build\",\"operations\":2}]}";
	char err[256] = {0};
	struct pbs_explain *ex = pbs_explain_parse(json, strlen(json), err, sizeof(err));
	char buf[512];
	char url[1024];
	char sha[128];

	if (ex == NULL) {
		fail("the kernel-shaped document failed to parse");
		return;
	}

	if (pbs_explain_version(ex, buf, sizeof(buf)) != 0)
		fail("version fusion failed for release 15");
	else
		expect_str("fused version carries the release", buf, "7.2.3-15");

	/* Two positional sources (ADR-0036): the second is a plain file
	 * copied into /build/extra, not a second mirror of the first. */
	expect_int("two sources", pbs_explain_source_count(ex), 2);
	if (pbs_explain_source(ex, 1, url, sizeof(url), sha, sizeof(sha)) != 0)
		fail("source 1 could not be read");
	else
		expect_str("second source sha", sha,
		           "a3fda92c6313292c48a4ad98728772aa156ea234bd048ae49811f6f5c171bc46");

	if (pbs_explain_requires(ex, "runtime", "package", buf, sizeof(buf)) != 0)
		fail("runtime packages failed");
	else
		expect_str("runtime packages", buf, "openssl libarchive curl");

	expect_str("upstream", pbs_explain_upstream(ex), "kernel.org");
	expect_str("toolchain", pbs_explain_toolchain(ex), "gcc");
	expect_str("toolchain reason", pbs_explain_toolchain_reason(ex),
	           "the kernel does not build with TCC");

	/*
	 * Capabilities by NAME (cix-build-system#162, closed). The name is
	 * the whole point: a count of 1 did not say whether the recipe
	 * asked for CAP_SYS_ADMIN or CAP_NET_ADMIN, which is why a
	 * non-zero count was refused at publish. The shape here matches
	 * `pkg_build_caps=`, so both recipe formats hand
	 * pkg_build_container_spec() the same string.
	 */
	if (pbs_explain_capabilities(ex, buf, sizeof(buf)) != 0)
		fail("named capabilities could not be read");
	else
		expect_str("capability names", buf, "CAP_SYS_ADMIN");

	/* The two embedder-owned facts (cix-build-system#161, closed).
	 * artifact_sha256 is the one that makes a conversion free: without
	 * it a converted recipe rebuilds from source on every install on
	 * every host, and 88 of 148 current recipes carry one. */
	if (pbs_explain_metadata(ex, "artifact_sha256", buf, sizeof(buf)) != 0)
		fail("artifact_sha256 could not be read out of metadata");
	else
		expect_str("artifact_sha256 from metadata", buf,
		           "c09de99506f24a17786da6507775c2fb3e5e3882ec25e234f85632ec5f4607a8");
	if (pbs_explain_metadata(ex, "changelog", buf, sizeof(buf)) != 0)
		fail("changelog could not be read out of metadata");
	else
		expect_str("changelog from metadata", buf, "7.2.3-15: real revision note");

	/* A key the platform does not understand is absent, not an error:
	 * CBS assigns metadata no meaning, so an unknown key is ordinary. */
	if (pbs_explain_metadata(ex, "no_such_key", buf, sizeof(buf)) != 0)
		fail("an unknown metadata key should read as absent");
	else
		expect_str("unknown metadata key", buf, "");

	pbs_explain_free(ex);
}

/*
 * An OLDER cbs, reporting a capability COUNT. This is a document cixd
 * can really be handed -- it execs whichever cbs is installed -- and
 * reading it as zero would drop a declared capability in silence. The
 * accessor answers 1 for it, distinctly from 0-means-none, and both
 * the publish and the build path refuse on that.
 */
static void test_legacy_capability_count(void)
{
	static const char *const json =
	    "{\"name\":\"legacy\",\"version\":\"1\",\"release\":1,\"architecture\":null,"
	    "\"sources\":[{\"name\":\"s\",\"urls\":[\"https://example.invalid/a.tar.gz\"],"
	    "\"sha256\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"}],"
	    "\"requires\":{},\"capabilities\":1,\"phases\":[]}";
	char err[256] = {0};
	struct pbs_explain *ex = pbs_explain_parse(json, strlen(json), err, sizeof(err));
	char buf[256];

	if (ex == NULL) {
		fail("the legacy-shaped document failed to parse");
		return;
	}
	expect_int("a capability COUNT is reported as such, not as none",
	           pbs_explain_capabilities(ex, buf, sizeof(buf)), 1);
	expect_str("and writes no names", buf, "");

	/* A count of ZERO is genuinely none, and must NOT be refused --
	 * an older cbs building a recipe that declares no capability is
	 * fine, and refusing it would break every such recipe. */
	pbs_explain_free(ex);
	{
		static const char *const zero =
		    "{\"name\":\"legacy\",\"version\":\"1\",\"release\":1,"
		    "\"architecture\":null,\"sources\":[{\"name\":\"s\","
		    "\"urls\":[\"https://example.invalid/a.tar.gz\"],\"sha256\":\"00112233445566"
		    "778899aabbccddeeff00112233445566778899aabbccddeeff\"}],"
		    "\"requires\":{},\"capabilities\":[],\"phases\":[]}";
		struct pbs_explain *z = pbs_explain_parse(zero, strlen(zero), err, sizeof(err));

		if (z == NULL) {
			fail("the zero-count document failed to parse");
			return;
		}
		expect_int("a count of zero is none, not a refusal",
		           pbs_explain_capabilities(z, buf, sizeof(buf)), 0);
		expect_str("and writes no names", buf, "");
		pbs_explain_free(z);
	}
}

static void test_first_url_of_several(void)
{
	/* CPDL allows several URLs per source (mirrors); cixd's model is
	 * one URL per positional source. The first is taken and the rest
	 * ignored -- never concatenated, which would produce a URL that
	 * fetches nothing and a checksum failure naming the wrong cause. */
	static const char *const json =
	    "{\"name\":\"m4\",\"version\":\"1.4.20\",\"release\":2,"
	    "\"sources\":[{\"name\":\"m4\",\"urls\":[\"https://mirrors.kernel.org/m4.tar.gz\","
	    "\"https://ftp.gnu.org/gnu/m4/m4.tar.gz\"],\"sha256\":\"abc\"}],"
	    "\"requires\":{},\"capabilities\":0,\"phases\":[]}";
	char err[256] = {0};
	struct pbs_explain *ex = pbs_explain_parse(json, strlen(json), err, sizeof(err));
	char url[1024];
	char sha[128];

	if (ex == NULL) {
		fail("the multi-URL document failed to parse");
		return;
	}
	if (pbs_explain_source(ex, 0, url, sizeof(url), sha, sizeof(sha)) != 0)
		fail("multi-URL source could not be read");
	else
		expect_str("first URL is taken", url, "https://mirrors.kernel.org/m4.tar.gz");

	/* An empty requires object must answer "" for every question,
	 * not fail. */
	{
		char buf[64];

		if (pbs_explain_requires(ex, "build", "tool", buf, sizeof(buf)) != 0)
			fail("empty requires was reported as an error");
		else
			expect_str("empty requires", buf, "");
	}
	pbs_explain_free(ex);
}

static void test_rejections(void)
{
	char err[256];
	struct pbs_explain *ex;

	/* Not JSON at all. The likeliest real cause is cbs writing a
	 * diagnostic to stdout, or the exec failing and leaving an empty
	 * buffer -- both must be refused rather than mapped onto an empty
	 * recipe that fails later on a name mismatch. */
	err[0] = '\0';
	ex = pbs_explain_parse("not json", 8, err, sizeof(err));
	if (ex != NULL) {
		fail("non-JSON input parsed");
		pbs_explain_free(ex);
	} else if (err[0] == '\0') {
		fail("non-JSON input set no error message");
	}

	/* Empty, which is what a failed exec leaves behind. */
	err[0] = '\0';
	ex = pbs_explain_parse("", 0, err, sizeof(err));
	if (ex != NULL) {
		fail("empty input parsed");
		pbs_explain_free(ex);
	} else if (err[0] == '\0') {
		fail("empty input set no error message");
	}

	/*
	 * Valid JSON, but missing `release`. CPDL's validate_package()
	 * requires exactly one release declaration, so a document without
	 * one did not come from `cbs explain --json` -- and accepting it
	 * would fuse a version ending in "-0".
	 */
	{
		static const char *const no_release = "{\"name\":\"x\",\"version\":\"1\"}";

		err[0] = '\0';
		ex = pbs_explain_parse(no_release, strlen(no_release), err, sizeof(err));
		if (ex != NULL) {
			fail("a document with no release parsed");
			pbs_explain_free(ex);
		}
	}

	/* Valid JSON, no name. */
	{
		static const char *const no_name = "{\"version\":\"1\",\"release\":1}";

		err[0] = '\0';
		ex = pbs_explain_parse(no_name, strlen(no_name), err, sizeof(err));
		if (ex != NULL) {
			fail("a document with no name parsed");
			pbs_explain_free(ex);
		}
	}
}

static void test_truncation_is_refused(void)
{
	/* Every accessor must refuse rather than truncate. A truncated
	 * checksum is the dangerous one: it would be compared against a
	 * fetched file and could only ever mismatch, reporting a
	 * corrupt download for a recipe that was fine. */
	char err[256] = {0};
	struct pbs_explain *ex = pbs_explain_parse(ZSTD_JSON, strlen(ZSTD_JSON), err, sizeof(err));
	char tiny[8];
	char url[1024];
	char sha[128];

	if (ex == NULL) {
		fail("fixture failed to parse in the truncation test");
		return;
	}
	if (pbs_explain_version(ex, tiny, 4) == 0)
		fail("a version that does not fit was accepted");
	else if (tiny[0] != '\0')
		fail("a refused version left content in the buffer");

	if (pbs_explain_source(ex, 0, tiny, sizeof(tiny), sha, sizeof(sha)) == 0)
		fail("a URL that does not fit was accepted");
	if (pbs_explain_source(ex, 0, url, sizeof(url), tiny, sizeof(tiny)) == 0)
		fail("a checksum that does not fit was accepted");

	if (pbs_explain_requires(ex, "build", "tool", tiny, sizeof(tiny)) == 0)
		fail("a tool list that does not fit was accepted");
	else if (tiny[0] != '\0')
		fail("a refused tool list left content in the buffer");

	/* Out-of-range index. */
	if (pbs_explain_source(ex, 7, url, sizeof(url), sha, sizeof(sha)) == 0)
		fail("an out-of-range source index was accepted");
	if (pbs_explain_source(ex, -1, url, sizeof(url), sha, sizeof(sha)) == 0)
		fail("a negative source index was accepted");

	pbs_explain_free(ex);
}

static void test_null_safety(void)
{
	char buf[64];

	/* Every accessor is called on a NULL handle somewhere in an error
	 * path; none may crash and none may return NULL for a string. */
	expect_str("name of NULL", pbs_explain_name(NULL), "");
	expect_str("upstream of NULL", pbs_explain_upstream(NULL), "");
	expect_str("toolchain of NULL", pbs_explain_toolchain(NULL), "");
	expect_str("format of NULL", pbs_explain_format(NULL), "");
	expect_str("toolchain reason of NULL", pbs_explain_toolchain_reason(NULL), "");
	expect_int("source count of NULL", pbs_explain_source_count(NULL), 0);
	{
		char buf[64];

		expect_int("capabilities of NULL", pbs_explain_capabilities(NULL, buf, sizeof(buf)),
		           -1);
		expect_int("metadata of NULL", pbs_explain_metadata(NULL, "k", buf, sizeof(buf)), -1);
	}
	expect_int("phase count of NULL", pbs_explain_phase_count(NULL), 0);
	if (pbs_explain_version(NULL, buf, sizeof(buf)) == 0)
		fail("version of NULL succeeded");
	if (pbs_explain_requires(NULL, "build", "tool", buf, sizeof(buf)) == 0)
		fail("requires of NULL succeeded");
	pbs_explain_free(NULL);
}

/*
 * The declared artifact format (ADR-0307 clause 1).
 *
 * Both legal values and the absent case, because the daemon does three
 * different things with them: cixpkg is published as a .cixpkg,
 * "tar.gz" is REFUSED at publish (cbs build will not execute it), and
 * absent means the document was derived by an engine older than
 * v0.1.26 and has to be re-derived rather than guessed at. A reader
 * that collapsed any two of those would take the wrong branch silently.
 */
static void test_declared_format(void)
{
	static const char *const CIXPKG =
	    "{\"name\":\"p\",\"version\":\"1\",\"release\":1,\"format\":\"cixpkg\","
	    "\"sources\":[{\"name\":\"p\",\"urls\":[\"https://e/p.tar.gz\"],\"sha256\":\"aa\"}]}";
	static const char *const TARGZ =
	    "{\"name\":\"p\",\"version\":\"1\",\"release\":1,\"format\":\"tar.gz\","
	    "\"sources\":[{\"name\":\"p\",\"urls\":[\"https://e/p.tar.gz\"],\"sha256\":\"aa\"}]}";
	static const char *const NULLFMT =
	    "{\"name\":\"p\",\"version\":\"1\",\"release\":1,\"format\":null,"
	    "\"sources\":[{\"name\":\"p\",\"urls\":[\"https://e/p.tar.gz\"],\"sha256\":\"aa\"}]}";
	char err[256];
	struct pbs_explain *ex;

	ex = pbs_explain_parse(CIXPKG, strlen(CIXPKG), err, sizeof(err));
	if (ex == NULL)
		fail("a document declaring cixpkg failed to parse");
	else
		expect_str("declared cixpkg", pbs_explain_format(ex), "cixpkg");
	pbs_explain_free(ex);

	ex = pbs_explain_parse(TARGZ, strlen(TARGZ), err, sizeof(err));
	if (ex == NULL)
		fail("a document declaring tar.gz failed to parse");
	else
		expect_str("declared tar.gz", pbs_explain_format(ex), "tar.gz");
	pbs_explain_free(ex);

	/* A JSON null is not a declaration. It must read the same as an
	 * absent key -- "" -- and not as the string "null", which would
	 * sail past the daemon's cixpkg comparison as an unknown format
	 * rather than being caught as a missing one. */
	ex = pbs_explain_parse(NULLFMT, strlen(NULLFMT), err, sizeof(err));
	if (ex == NULL)
		fail("a document with a null format failed to parse");
	else
		expect_str("format declared null", pbs_explain_format(ex), "");
	pbs_explain_free(ex);
}

int main(void)
{
	printf("=== PBS recipe identity (ADR-0305) ===\n");
	test_happy_path();
	test_release_and_runtime();
	test_legacy_capability_count();
	test_first_url_of_several();
	test_rejections();
	test_truncation_is_refused();
	test_null_safety();
	test_declared_format();

	if (g_failures != 0) {
		printf("PBS RECIPE TEST: FAIL (%d failure(s))\n", g_failures);
		return 1;
	}
	printf("PBS RECIPE TEST: PASS\n");
	return 0;
}
