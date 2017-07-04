#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <string.h>
#include <math.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
#include <clob/clob_val.h>
#include "dfp754_d64.h"
#include "bot.h"
#include "nifty.h"

#define strtoqx		strtod64
#define strtopx		strtod64
#define qxtostr		d64tostr
#define pxtostr		d64tostr

#define floorpx		floord64

struct quo_s {
	px_t b;
	px_t a;
};

struct acc_s {
	qx_t base;
	qx_t term;
};

static double noise = M_SQRT1_2;
static double mu = 0, sg = 1.;
static double pos_cor = 1.;
static px_t min_tick = 0.01dd;
static qx_t quo_size = 500.dd;
static qx_t max_pos = 1000000.dd;
static double(*calc_qt)(double);

static clob_oid_t coid[NSIDES];

static struct quo_s quo = {-1.dd, 1.dd};
static struct acc_s acc = {0.dd, 0.dd};
static unsigned int tra;

static const char *cont;
static size_t conz;
#define INS		.ins = cont, .inz = conz


static inline __attribute__((const, pure)) double
gauss_cdf(double x)
{
	return 0.5 * (1. + erf(M_SQRT1_2 * x));
}

static inline __attribute__((const, pure)) double
gauss_pdf(double x)
{
#define M_SQRT1_2PI	0.3989422804014327
	return M_SQRT1_2PI * exp(-0.5 * x * x);
}

static double
alpha2(double x, double q)
{
	const double N_q = gauss_pdf(q);
	const double Phi_q = gauss_cdf(q);
	double f = x * N_q * (N_q - q * (1 - Phi_q));
	f /= (1 + x) * (1 - Phi_q) * (1 - Phi_q);
	return 1 - f;
}

static double
beta2(double x, double q)
{
	const double N_q = gauss_pdf(q);
	const double Phi_q = gauss_cdf(q);
	double f = 2 * x * q * N_q;
	f /= (1 + x) * (2 * Phi_q - 1);
	return 1 - f;
}

static double
calc_qt_zp(double r2)
{
	double q = 0.;

	for (size_t i = 0U; i < 10U; i++) {
		const double N_q = gauss_pdf(q);
		const double Phi_q = gauss_cdf(q);

		q = r2 / (1 + r2) * N_q / (1 - Phi_q);
	}
	return q;
}

static double
calc_qt_myo(double r2)
{
	double q = 0.;

	for (size_t i = 0U; i < 10U; i++) {
		const double N_q = gauss_pdf(q);
		const double Phi_q = gauss_cdf(q);

		q = (1. + r2) * (1 - Phi_q) / N_q;
	}
	return q;
}

static struct quo_s
mm(int s)
{
/* Using fix point evaluation, either calc_qt_zp() or calc_qt_myo()
 * s < 0, trader sold
 * s == 0, trader did nothing
 * s > 0, trader bought */
	const double rho = sg * pos_cor / noise;
	const double r2 = rho * rho;
	const double qt = calc_qt(r2);
	const double N_qt = gauss_pdf(qt);
	const double Phi_qt = gauss_cdf(qt);
	double alpha, kappa, d, db, da;
	px_t b, a;

	if (s) {
		/* handle long and short */
		alpha = sqrt(alpha2(r2, qt));

		kappa = sqrt(r2 / (1. + r2));
		kappa *= N_qt;
		kappa /= 1 - Phi_qt;
		/* sign kappa according to S */
		kappa *= s > 0 ? 1. : -1.;
	} else /*if (s == 0)*/ {
		/* only calc beta and name it alpha */
		alpha = sqrt(beta2(r2, qt)), kappa = 0.;
	}

	/* reconstruct best spread */
	if ((d = qt * noise / sqrt(1 + r2)) < (double)min_tick) {
		/* don't update */
		;
	} else {
		/* update beliefs */
		mu += kappa * sg;
		sg *= alpha;
	}
	/* position spread according to base */
	db = (asinh((double)acc.base / max_pos) + 1) * 0.5;
	da = 1 - db;

	//send_info("mu %f  sg %f  rho %f  -> %f (%f~%f)\n", mu, sg, rho, d, db, da);
	b = quantizepx((px_t)(mu - db * (2 * d)), min_tick);
	a = quantizepx((px_t)(mu + da * (2 * d)), min_tick);
	if (a - b < min_tick) {
		b /= min_tick;
		a /= min_tick;
		b = floorpx(b) * min_tick;
		a = floorpx(a) * min_tick + min_tick;
	}

#if 0
/* for higher level quotes */
	for (double l = 2; l < 10; l += 1.) {
		const double d2 = qt * noise * sqrt(l) / sqrt(1 + r2);
		px_t b2 = quantizepx(mu - db * (2 * d2), min_tick);
		px_t a2 = quantizepx(mu + da * (2 * d2), min_tick);
		b2 /= min_tick;
		a2 /= min_tick;
		b2 = floord32(b2) * min_tick;
		a2 = floord32(a2) * min_tick + min_tick;
		
		send_info("sg2 %f  -> %f  (%f/%f)\n", sg * alpha, d2, (double)b2, (double)a2);
	}
#endif
#if 0
/* recalc noise */
	static double m0, m1, m2;
	with (double delta = mu - m1) {
		m1 += delta / (m0 += 1);
		m2 += (mu - m1) * delta;
	}
	/* normalise */
	if (m0 > 100) {
		noise = sqrt(m2 / m0);
	}
	send_info("real_mu %f  real_sg %f\n", m1, m2 / m0);
#endif
	return (struct quo_s){b, a};
}


static void
ochan_cb(bot_t UNUSED(b), omsg_t m)
{
	switch (m.typ) {
		int dir;

	case OMSG_OID:
		coid[m.oid.sid] = m.oid;
		break;
	case OMSG_FIL:
		break;
	case OMSG_ACC:
		if (0) {
			;
		} else if (m.exa.base > acc.base) {
			dir = tra & 0b01U ? 0 : -1;
			tra |= 0b01U;
			acc.base = m.exa.base;
		} else if (m.exa.base < acc.base) {
			dir = tra & 0b10 ? 0 : 1;
			tra |= 0b10U;
			acc.base = m.exa.base;
		} else {
			puts("UNK FIL");
			break;
		}
		/* calc new quote */
		if (dir) {
			quo = mm(dir);
		}
		break;
	default:
		break;
	}
	return;
}

static void
hbeat_cb(bot_t b)
{
/* generate a random trade */
	/* cancel old guys */
	add_omsg(b, (omsg_t){OMSG_CAN, INS, .oid = coid[SIDE_BID]});
	add_omsg(b, (omsg_t){OMSG_CAN, INS, .oid = coid[SIDE_ASK]});
	/* recalc quo if there hasn't been a trade */
	if (!tra) {
		quo = mm(0);
	}

	add_omsg(b, (omsg_t){OMSG_BUY, INS,
				 .ord = (clob_ord_t){TYPE_LMT,
						     .qty = {quo_size, 0.dd},
						     .lmt = quo.b}});
	add_omsg(b, (omsg_t){OMSG_SEL, INS,
				 .ord = (clob_ord_t){TYPE_LMT,
						     .qty = {quo_size, 0.dd},
						     .lmt = quo.a}});
	bot_send(b);
	/* reset trading indicator */
	tra = 0U;
	return;
}


#include "dasbot.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	const char *host = "localhost";
	double freq = 1.0;
	int rc = 0;
	bot_t b;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if (argi->nargs) {
		cont = *argi->args;
		conz = strlen(cont);
	}

	if (argi->host_arg) {
		host = argi->host_arg;
	}

	if (argi->freq_arg) {
		freq = strtod(argi->freq_arg, NULL);
	}

	calc_qt = calc_qt_zp;

	/* initialise the bot */
	if (UNLIKELY((b = make_bot(host)) == NULL)) {
		goto out;
	}

	b->timer_cb = hbeat_cb;
	bot_set_timer(b, freq, freq);

	b->ochan_cb = ochan_cb;

	/* go go go */
	rc = run_bots(b) < 0;

	kill_bot(b);

out:
	yuck_free(argi);
	return rc;
}
