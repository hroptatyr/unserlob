library(data.table)
library(ggplot2)
library(scales)

asinh_trans <-
        trans_new(name = 'asinh',
                transform = function(x) asinh(x),
                inverse = function(x) sinh(x));

asinh_breaks <- function(x) {
        b <- 10^(-10:10);
        b <- sort(c(-b, -10*b, 0, b, 10*b));

        rng <- range(x, na.rm = TRUE);

        nx1 <- pmax(Filter(function(x) x <= rng[1], b))
        nx2 <- pmin(Filter(function(x) x >= rng[2], b))
        c(nx1, Filter(function(x) x < rng[2], Filter(function(x) x > rng[1], b)), nx2);
}

asinh_mbreaks <- function(x) {
        b <- 10^(-10:10);
        b <- sort(c(-b, -10*b, 0, b, 10*b) / 2);

        rng <- range(x, na.rm = TRUE);

        nx1 <- pmax(Filter(function(x) x <= rng[1], b))
        nx2 <- pmin(Filter(function(x) x >= rng[2], b))
        c(nx1, Filter(function(x) x < rng[2], Filter(function(x) x > rng[1], b)), nx2);
}

scale_y_asinh <- function(...) {
        scale_y_continuous(..., trans = asinh_trans,
		breaks = asinh_breaks, minor_breaks = asinh_mbreaks);
}

taq_top <- function(file)
{
	fread(file) -> taq;
	taq[, V1 := as.POSIXct(V1, origin="1970-01-01")];
	ggplot(taq[V4=="TRA"], aes(V1, V6, colour=V5)) + geom_step() -> p;
	p + theme_light() + scale_y_asinh() -> p;
	print(p);
	dev.off();
}
