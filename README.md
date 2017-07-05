unserlob
========

A trading venue to research maker and taker algorithms.

Depends on [clob][1] for a limit order book and matching engine
and [libev][2] for an event loop.


Topology
--------

An exchange listens on `tcp/7979` for market participants.  This
is the (private) pre-trade and post-trade communication channel,
i.e. the client can send orders or the server sends execution
reports.

The exchange publishes quote messages on `ff05::134/7978` (the
unserding multicast network).  Public post-trade information
goes to `ff05::134/7979`.

Optionally clients can send a drop-copy of their pre-trade
messages to `ff05::134/7977`.

The tool `lobmon` subscribes to all 3 groups and repeats all
messages in the console.


Wire protocol
-------------

To facilitate rapid development and uphold readability unserlob
uses a text-based wire protocol:

    quote messages (ff05::134/7978):
    <- "A1" \t INS \t P \t Q \n   top-level ask of INS is Q at P
    <- "B1" \t INS \t P \t Q \n   top-level bid of INS is P for Q
    <- "A2" \t INS \t P \t Q \n   quantity at P of INS is now Q
    <- "B2" \t INS \t P \t Q \n   P is now for quantity Q of INS
    <- "TRA" \t INS \t P \t Q \n  INS has been traded P for Q
    <- "AUC" \t INS \t P \t I \n  Theoretical auction price of
                                  INS is P at an imbalance of I

All quote messages have exactly 4 columns.  The arrow indicates
that these messages are all incoming from a client's point of
view.

    order messages (tcp/7979 and ff05::134/7979):
    -> "BUY" \t INS \t Q[+H] [\t P]
       buy Q+H quantities of INS, pay either P or market price
       (if omitted), only Q quantities are displayed
    -> "SEL" \t INS \t Q[+H] [\t P]
       sell Q+H quantities of INS, ask for either P or market
       price (if omitted), only Q quantities are displayed
    <- "OID" \t INS \t X
       order for instrument was accepted and order id is X
    <- "FIL" \t INS \t Q \t P \t U [\t X]
       order X has Q quantities filled at P, contra firm is U,
       the X might be missing e.g. if market orders get filled
       instantaneously without making it into the book
    <- "ACC" \t INS \t B \t T
       current account balance is B base quantities and T terms
    -> "CAN" \t INS \t X
       request cancellation of order X
    <- "KIL" \t INS \t X
       cancellation confirmed, order X has been killed
    <- "NOK" \t INS \t X
       cancellation rejected, order X does not exist or has not
       been killed

An arrow `<-` indicates that this message is sent from the
exchange to the client, an arrow `->` denotes a client initiated
message.

Messages `BUY`, `SEL` and `CAN` as well as their responses
(`OID`, `KIL` or `NOK`) can optionally appear on the pre-trade
channel (`ff05::134/7977`).


C API
-----

From within C bots can use `liblol` to connect to an exchange
and do their trading.  The following skeleton demonstrates the
API (error checking has been omitted for clarity):

    #include "bot.h"

    static void orders(bot_t b, omsg_t msg)
    {
    /* called on pre-trade or post-trade confirmations,
     * fills, order-accepts, etc. */
            switch (msg.typ) {
            ...
            }
            return;
    }
    
    static void quotes(bot_t b, qmsg_t msg)
    {
    /* called when there are new quotes or trades */
            switch (msg.typ) {
            ...
            }
            return;
    }
    
    static void heartb(bot_t b)
    {
    /* called 10.0 seconds after start-up and
     * every 5.0 seconds thereafter */
            ...
            return;
    }
    
    int main(...)
    {
            ...
            bot_t b = make_bot(HOST);
    
            b->ochan_cb = orders;
            b->qchan_cb = quotes;
            b->timer_cb = heartb;
            bot_set_timer(b, 10.0, 5.0);
            /* run the bot */
            run_bots(b);
    
            /* clean up */
            kill_bot(b);
            return 0;
    }


Exchanges
---------

Two market models have been implemented, continuous trading and
continuous auction.  A venue running the former can be
instatiated with the `lobot(1)` program, a venue running the
latter with `cabot(1)`.  Both programs take the names of
instruments traded on the exchange.  Orders are accepted only
for those instruments.

  [1]: https://github.com/hroptatyr/clob
  [2]: http://software.schmorp.de/pkg/libev.html
