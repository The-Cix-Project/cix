/*
 * ADR-0254: OpenPGP clearsign verification.
 *
 * The fixtures are embedded rather than fetched: this must run in a
 * build container with no network and no gpg, and a test that needs
 * either is a test that silently stops running.
 *
 * The signed document is deliberately SIXTY-FIVE lines. A short one
 * would pass against a broken implementation: canonicalisation turns
 * LF into CRLF, so a k-line document grows by up to k bytes, and the
 * original malloc(len + 2) was an underestimate for anything past two
 * lines. Against kernel.org's real ~200-line sha256sums.asc it overran
 * the heap outright. A four-line fixture fits in the slack and proves
 * nothing.
 */
#include "pgpverify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

static void fail(const char *what, const char *detail)
{
	fprintf(stderr, "  FAIL: %s (%s)\n", what, detail);
	g_failures++;
}

static const char TEST_KEY[] =
	"-----BEGIN PGP PUBLIC KEY BLOCK-----\n"
	"\n"
	"mQENBGqdWf8BCADS++Lhn5N9DbL0VrCDc9nhgXFVlcFF+QzNQCkt1XIMaBnaYw0b\n"
	"9bQIB6cWXhUFanNrAZp08lFLK4Y8d8qzVVs3oUGbyFvFNGAIiU6cgLs+aXG0NPT3\n"
	"iRKEU7cZKcqGTBNXMWdidS7GXHESl5IyizF5z7GJdo8fKuCHoiRNdnhaE9rOQJPI\n"
	"w9UgKMKc4+rBImg4+YMQEd6+5EqwwZQgPsmIIHj2y6X605Ir8PNoRGMvODd8DUnz\n"
	"4leQp1usVGuD2+knTfvgfFh0jHO7ZDIElynsO6fh1G/Bs8EJOIsDUDHpkcAxqOQY\n"
	"Jn7m4HmcUVuEAKh8g/XkCzSgOcLW7E7neqZXABEBAAG0JkNpeCBUZXN0IFNpZ25l\n"
	"ciA8dGVzdEBleGFtcGxlLmludmFsaWQ+iQFOBBMBCgA4FiEEW35PLl5vWE0GgAy2\n"
	"37YU6BLyYfoFAmqdWf8CGy8FCwkIBwIGFQoJCAsCBBYCAwECHgECF4AACgkQ37YU\n"
	"6BLyYfq8ywf/cXNA1CFoVPuKsX50cHiY9eRVQ/6v/8YLWxpdIwINRk8tHRwAelto\n"
	"07rc7FSlHgxboEz3lT1yZFQ/4Pj+x86Dh6il3AfcLj4UklK4Ck36ZhCUXXejnga6\n"
	"OJD/p3YwWp9gx3R3EyxyDuozaQasDZHjxXuIxvveTfygwD+Olc2lLjk3LNnVuRbN\n"
	"wEVJ2PotBP0+9EaqLLj38IkcA00QdnqFcNbcFLKCR8DG81Cv5xAscGRzpNBU77LZ\n"
	"VLSN7++24hNTcSl0kbPntI3thFjsoZQyPOKx9Ebg1doGPZ+qlyCq6I5GnV1NJ/aQ\n"
	"SqcKRvm+VixuOFeyZAVetf4KOBOoRliPQQ==\n"
	"=mt0t\n"
	"-----END PGP PUBLIC KEY BLOCK-----\n"
	"\n";

static const char SIGNED_DOC[] =
	"-----BEGIN PGP SIGNED MESSAGE-----\n"
	"Hash: SHA256\n"
	"\n"
	"# a checksum listing, deliberately long enough that a CRLF-growth\n"
	"# overflow in canonicalisation actually overruns the allocation\n"
	"- - this line is dash-escaped and must survive unescaping\n"
	"trailing whitespace on the next line is not signed   \n"
	"1286f31ef77039563512171ea7072f7c6a481852947939645f21d6f8fd174a5a  linux-fixture-0.tar.xz\n"
	"dde23ff805667659ae248f40740a4b9e95d8c8a920d97220ee7e74cd08b37493  linux-fixture-1.tar.xz\n"
	"d24a05e792729edb1792a794aacde68849cc808f6b20d82fbab1597b3e428a02  linux-fixture-2.tar.xz\n"
	"1b5ba6e645ef4cbbe4581c3056a111820b96d809ceeb960c27b6110b3bf1259f  linux-fixture-3.tar.xz\n"
	"30cb30f95c3979d3a301d5a84cdb4001880877be4cd48f036ef4f4c1315f6f35  linux-fixture-4.tar.xz\n"
	"ed9ba136fe15b66cd3c5b6d9f2936ef256fcd4797be539159849d648b8cb0901  linux-fixture-5.tar.xz\n"
	"f8375c1bb6f430cf65d282d4155cee954dc4af71d1b1f434d6ff9b3da4e1bc38  linux-fixture-6.tar.xz\n"
	"d0b5a8b22c671da9b231dc90e8143e6714e414b25668cfd4a949f9678b943810  linux-fixture-7.tar.xz\n"
	"ff507b884683507faa34bcc9fd4f382039a19bc1e2467d9df1f95b86a64805c3  linux-fixture-8.tar.xz\n"
	"d844a12e4b4365ce38e93d6d73a94fd96ab733f83693929b2bbbe64584313eaa  linux-fixture-9.tar.xz\n"
	"d4f6d358e3648e0935e623bc3e7428f61e81967bb9cdeb2744ad10cecc9af856  linux-fixture-10.tar.xz\n"
	"bedfc06cb2aa623c7d90af865cb17df31cce655f2dcc728c9ecf71b7fca153f4  linux-fixture-11.tar.xz\n"
	"d5e643e95de1ad28a89fde796edce87542d393c69989f604dd1db813797a45b0  linux-fixture-12.tar.xz\n"
	"b641932befd907f8e26558b7e3426bf0ac24c690a84f4caef98d8e51cbadb224  linux-fixture-13.tar.xz\n"
	"df2494f8d674227b4c1958f34df5f27bb0c7853421aafe2928b1221d60239d37  linux-fixture-14.tar.xz\n"
	"3416716041d4f6259ebe146ab233d763152f072f432e7ff55712aace6a4e4b29  linux-fixture-15.tar.xz\n"
	"92765d40e6f3f3dc3970b8446da713184a4735a9cc86480911a4cb2ffa596051  linux-fixture-16.tar.xz\n"
	"66643c85e19ac786162fb502c33823d92ca3784af8ca6cceb75e5311c5bb6fe7  linux-fixture-17.tar.xz\n"
	"d2a185a03ae2c85033d960ff5e9cf0fc8478de85f2d3f3860027f810ee27f4c3  linux-fixture-18.tar.xz\n"
	"59330df185bd488300dd690e4aa08a9a9157fe5b529424572b6db1f46c0943a9  linux-fixture-19.tar.xz\n"
	"f281a6ad01e127a3bf0213e04ff866e6e6db711065b9fdee54ff4beb44554748  linux-fixture-20.tar.xz\n"
	"428c272d4c370d9ffc00c21c50b7df3e7d635ece6d4165521ef88a2758ce7f9c  linux-fixture-21.tar.xz\n"
	"fcfebc962f833ea6bcac2a16a7265cddb06a34dfc03eb9261317d6fced1dd5e5  linux-fixture-22.tar.xz\n"
	"4c6ea362c8983b6bc2f8ae44945dbf04a57e717aef7699219cbc8fede589db95  linux-fixture-23.tar.xz\n"
	"d884a1cebd896bb165e784a3a40d7343a4794c4ca87e7eb02024bf8e48faaab0  linux-fixture-24.tar.xz\n"
	"abcf01d9d69aeca6fe231af1c26fd4ee46daa40e352e02ba152b55e2b46ebbcd  linux-fixture-25.tar.xz\n"
	"cb0541f6eccb60d767f5f9fdf42077093e7f1b6cd57550cbdabf5a9083f5fe85  linux-fixture-26.tar.xz\n"
	"59ec6a8d474c8a1a23ad154d64d402993b6be937833da1781f6370334433d9ac  linux-fixture-27.tar.xz\n"
	"a23fe449dd7f326a70652f7ea798a86077b356b94fc89e57e452067983fcc892  linux-fixture-28.tar.xz\n"
	"8d9b2ea33b1d0dbba2d78b289e2af5a91a723c1d27af1eac23a5a4c4de5ea9e8  linux-fixture-29.tar.xz\n"
	"3abca2b6c150096337250e18ce0ce808ba87a591cb90acc50db5c5e4e11c6a4f  linux-7.2.3.tar.xz\n"
	"a3476626a8b3512e8167a56d06be99d28e839f6ca338e8d843fb75d439fcf735  linux-fixture-30.tar.xz\n"
	"9b868f99d5927431597381a76fc0963d06b2c2f6b32b921ad4feeb9fb29179b0  linux-fixture-31.tar.xz\n"
	"be384c5227f5c918ad2791f450db9b5877968fb119e59a11f8cdf7473fa85bcd  linux-fixture-32.tar.xz\n"
	"3bc3f6d98e7e419f06d01e68f9a42a96b9ab082d6927fb5637cb66a967a9060a  linux-fixture-33.tar.xz\n"
	"7a50e1a2351fd44450d8d0eca7ad86df4af6b6d2cb1f22d002d760f97c721c70  linux-fixture-34.tar.xz\n"
	"7b28866cbe9013a5b3c39917163d7f81050f70a3b2e9060060b732bf7d00c9bd  linux-fixture-35.tar.xz\n"
	"9c386ec512c6b2d946b93b4d655b985e3779555300ec0fd8c797e37eedae55ca  linux-fixture-36.tar.xz\n"
	"7d7b8f5a7b7f9213e2d5d9d638a79f9ef918b0f4d7f71013778a63ef85089ca3  linux-fixture-37.tar.xz\n"
	"30de1498015e8bff2363a6c27abf4142b1eeaa6c13ab2f230b3819a3c5297285  linux-fixture-38.tar.xz\n"
	"74ba20e70fc2ffa980e017bc6d688f13b35053a29c85de774c8c2e4aff0b134c  linux-fixture-39.tar.xz\n"
	"136a5578b956dd2fb06e0971ebba091c8d7bb65289ad25d4e2c6303c58657ba4  linux-fixture-40.tar.xz\n"
	"37bc981da80babb3e881bb6fe5da72023f049f278a792cf5b59beb2450c864de  linux-fixture-41.tar.xz\n"
	"552bd5015344c79482cb95f17ca095d05b601ef28ccd546aab8dc22344a6d89b  linux-fixture-42.tar.xz\n"
	"f45f8ab065075afca6a88b5d9ca17e41d0ed4113426eff5a202c0eeb997695b8  linux-fixture-43.tar.xz\n"
	"bdc0df1120d8c920a73835305c8b86e29b555c7269616e24e2fe191143c34626  linux-fixture-44.tar.xz\n"
	"d8c50c501e74f977f95527e99a902219b937b667fd9e10f3bd6a71b94938587e  linux-fixture-45.tar.xz\n"
	"8de6b53a87ca8e5bddfbea40c087d0fa2bcdb4ea78bae6931568f45f1bceb030  linux-fixture-46.tar.xz\n"
	"ee687a4369453762b9b72a157b166b71e483f6226357c9747413cb992ccc60c9  linux-fixture-47.tar.xz\n"
	"a5b0f38b6a00d5ea2c1c4be5c2dd770d122c2a493b74c7e73db10869da8a5c26  linux-fixture-48.tar.xz\n"
	"d87f0de51fe5efa26bd7ad46ea16c0d9dd0a57e8d6517afdc5d8f839259ad474  linux-fixture-49.tar.xz\n"
	"b0b625f76297927d7ea241de5afd543f470fd41accb1787b85911e8d2feea4b4  linux-fixture-50.tar.xz\n"
	"4ab150d671471c5978bff7b2a8a6343e59893023ed9feb41eebff499e2086ebb  linux-fixture-51.tar.xz\n"
	"cc7a4f7506d329cddbb6cae4833d877a6b1cafcfff743f40acbdc78b5b122136  linux-fixture-52.tar.xz\n"
	"88991e942e73dd275afe00b03fa187d1836378c07683633fd902c35defe0172b  linux-fixture-53.tar.xz\n"
	"abe67fcf1d78623ae031f12948b770bae447600ee2bc85d1a19c4f4d75993e07  linux-fixture-54.tar.xz\n"
	"3e0700e5fbb2afaf3c7d0aaa1fbad795f2b93c32fe2350a339a15e4e723cd7d4  linux-fixture-55.tar.xz\n"
	"3bfce7b7456067ba8cbffa6cd05748ceb46041a2d3a28ba860f87fac50408b02  linux-fixture-56.tar.xz\n"
	"115e32922ddef7b7e0b609cb087af27a0c0968bf9b6a38b3fafeef5b3ad00221  linux-fixture-57.tar.xz\n"
	"5dbb6e4dbb7812e8e95ac007fedb79a6a7ccee8bfd1491588ac970ae8bbab627  linux-fixture-58.tar.xz\n"
	"294cb90115c40b5ad7ececf5aa3d42d3305316c1b0c823eeb24568e29402b937  linux-fixture-59.tar.xz\n"
	"-----BEGIN PGP SIGNATURE-----\n"
	"\n"
	"iQEzBAEBCAAdFiEEW35PLl5vWE0GgAy237YU6BLyYfoFAmqdWxkACgkQ37YU6BLy\n"
	"YfpBRQf8CyR4h3LWwc3BIvSh+YnU0Rda8js+iq+wuoeXiPV2xJOE6LzOS+AQknfG\n"
	"uBvLwp+PLqrqpgrQ8cpMHZw5ESFwnsAis9KrHTMkEopeTOMP3/NHn56+QzOmZJ6G\n"
	"8BjK4C7oPzoGzHbyqQjpknmCiX5W2di3Wf+EODYNGl3eRHCTAuuZbbLfRwrA4roT\n"
	"SgqWLMgz1HubTVTqeRPyIuYlH3NorE+ujY5Lzeqanmp9KGLptP/aJxHyoon+0GS9\n"
	"liGvg1839pZi1FfaO/n7Zrs9WyEAMigpCatNgAT8Ep2N11AwKyVJFTE9vTk1rtI3\n"
	"v2OW3aBcv/+OuzJdjc/DhAzDEK1JWQ==\n"
	"=08zl\n"
	"-----END PGP SIGNATURE-----\n"
	"\n";

#define TEST_FPR "5B7E4F2E5E6F584D06800CB6DFB614E812F261FA"

static enum pgp_verify_result verify(const char *doc, const char *fpr, char **text)
{
	char err[256];

	err[0] = '\0';
	return pgp_clearsign_verify(doc, strlen(doc), TEST_KEY, sizeof(TEST_KEY) - 1, fpr, text, NULL,
	                            err, sizeof(err));
}

int main(void)
{
	char *text = NULL;
	char sha[65];
	char *tampered;
	char *truncated;
	enum pgp_verify_result r;
	size_t i;

	r = verify(SIGNED_DOC, TEST_FPR, &text);
	if (r != PGP_VERIFY_OK)
		fail("a valid clearsigned document did not verify", pgp_verify_result_name(r));
	else if (text == NULL)
		fail("verified but returned no text", "text is NULL");

	if (text != NULL) {
		if (strstr(text, "this line is dash-escaped and must survive unescaping") == NULL)
			fail("dash-escaped line was not unescaped", "marker absent");
		if (strstr(text, "not signed   ") != NULL)
			fail("trailing whitespace was not stripped", "spaces survived");
		if (strstr(text, "\r\n") == NULL)
			fail("canonical text is not CRLF", "no CRLF found");

		if (pgp_checksum_lookup(text, "linux-7.2.3.tar.xz", sha, sizeof(sha)) != 0)
			fail("checksum lookup failed on verified text", "not found");
		else if (strlen(sha) != 64)
			fail("checksum is not 64 hex characters", sha);
		if (pgp_checksum_lookup(text, "linux-does-not-exist.tar.xz", sha, sizeof(sha)) == 0)
			fail("lookup invented a checksum for an absent file", sha);
	}
	free(text);
	text = NULL;

	/* one altered hex digit -- the exact attack this file exists to stop */
	tampered = strdup(SIGNED_DOC);
	if (tampered == NULL)
		return 1;
	for (i = 0; tampered[i] != '\0'; i++) {
		if (strncmp(tampered + i, "  linux-7.2.3.tar.xz", 20) == 0) {
			tampered[i - 1] = (char)(tampered[i - 1] == 'a' ? 'b' : 'a');
			break;
		}
	}
	r = verify(tampered, TEST_FPR, &text);
	if (r != PGP_VERIFY_BAD_SIGNATURE)
		fail("a tampered checksum was not rejected", pgp_verify_result_name(r));
	if (text != NULL)
		fail("failed verification still returned text", "text is not NULL");
	free(tampered);

	text = NULL;
	r = verify(SIGNED_DOC, "0000000000000000000000000000000000000000", &text);
	if (r != PGP_VERIFY_WRONG_KEY)
		fail("a wrong pinned fingerprint was accepted", pgp_verify_result_name(r));

	text = NULL;
	r = verify(SIGNED_DOC, "", &text);
	if (r != PGP_VERIFY_WRONG_KEY)
		fail("an empty fingerprint was not refused", pgp_verify_result_name(r));

	truncated = strdup(SIGNED_DOC);
	if (truncated == NULL)
		return 1;
	truncated[strlen(truncated) - 200] = '\0';
	text = NULL;
	r = verify(truncated, TEST_FPR, &text);
	if (r == PGP_VERIFY_OK)
		fail("a truncated document verified", "should be malformed or bad");
	free(truncated);

	if (g_failures > 0) {
		fprintf(stderr, "PGP VERIFY: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("PGP VERIFY: ok (valid, canonicalisation, lookup, tampered, wrong key, no pin, "
	       "truncated)\n");
	return 0;
}
