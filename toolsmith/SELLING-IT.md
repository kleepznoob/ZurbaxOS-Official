# Selling Toolsmith

Two audiences here. The first half is for whoever holds the payment account
(a parent — under-18s can't open one). The second half is the honest business
picture.

---

## Part 1 — turning the shop on

**The paywall ships switched off.** Right now every tool is fully unlocked and
nothing says "buy". That is deliberate: a buy button that leads nowhere is
worse than no buy button. Two values switch the whole thing on.

### What a grown-up needs to do

1. **Make a Gumroad account** at gumroad.com. It needs a real name, a bank
   account and an over-18 identity. Gumroad acts as *merchant of record*, which
   in plain terms means they handle VAT and sales tax on the sale rather than
   you having to register for tax in twenty countries. That is the main reason
   to use them over raw Stripe for a first product.
2. **Create a new product.** Digital product, name it Toolsmith, price it at
   £9. Add the screenshots from `../screenshots/`.
3. **Turn on licence keys** in the product's settings. Gumroad then emails a
   unique key to every buyer.
4. **Copy the product ID.** It is in the product's settings page, and it is not
   the same thing as the URL.
5. **Open `js/license.js`** and fill in the two blanks at the top:
   ```js
   checkoutUrl: "https://YOURNAME.gumroad.com/l/toolsmith",
   productId:   "the-product-id-you-just-copied",
   ```
6. **Test it before telling anyone.** Make a 100%-off discount code, buy your
   own product with it, and paste the key into the unlock box. This step is not
   optional — see the warning below.
7. Push the change. The Free/Pro badge appears and the limits start applying.

> ⚠️ **The verification code has never been run against a real Gumroad
> product**, because there wasn't one to test against when it was written. It
> calls Gumroad's documented `/v2/licenses/verify` endpoint and handles the
> refunded, wrong-key and offline cases, but *do the test purchase in step 6
> before announcing anything*. If the call is blocked by the browser, the error
> message will say it couldn't reach the store.

### What Free and Pro actually get

| | Free | Pro |
|---|---|---|
| All six tools | yes | yes |
| Watermarks | none, ever | none, ever |
| Batch at once | 5 files | unlimited |
| Export size | 2400 px | full resolution |
| PDF pages | 20 | unlimited |

Free is generous on purpose. Anyone hitting those limits is doing real volume
work, and they are exactly the people for whom £9 is obviously worth it. A
crippled free tier would just stop the thing spreading.

---

## Part 2 — the honest business picture

### Anyone can break the lock, and that's fine

The licence check runs in the browser, so a determined person can edit it out in
about five minutes. **Every** indie tool sold this way has that property.
Fixing it properly means running a server, which costs money every month and
needs an account holder, for a product that has sold nothing yet.

It matters less than it sounds. People who would bother cracking a £9 tool were
never going to pay £9. The lock exists to give honest people a clear way to pay,
not to stop thieves. Revisit this if it ever sells enough to be worth it.

### Why £9 once, and not £3 a month

- It is the whole pitch. "Pay once" only beats Adobe if it is actually once.
- Subscriptions need churn handling, failed-card emails and cancellation flows.
  That is a business, not a project.
- £9 is an impulse buy. £29 makes people think, and thinking loses sales.
- Gumroad keeps roughly 10% plus payment processing — check their current rate,
  it has changed before. On £9 that leaves about £7.50.

### What it will realistically earn

| Visitors a month | Buyers (1–2%) | Roughly |
|---|---|---|
| 100 | 0–1 | £0–8 |
| 1,000 | 10–20 | £75–150 |
| 10,000 | 100–200 | £750–1,500 |

The top row is where everything starts, and most projects stay there because
nobody ever hears about them. **The hard part is not the software, it is the
100 visitors.** The software is already done.

### Getting those first visitors

The strategy that fits a free-tool-with-paid-upgrade:

1. **Each tool is its own front door.** People search "resize lots of images at
   once" or "remove background from picture", not "creative suite". The URLs
   `#batch` and `#cutout` link straight to the right tool — use them.
2. **Show, don't describe.** A 20-second screen recording of 60 photos being
   resized in one drop does more than any paragraph.
3. **Go where the problem gets complained about**: r/DesignPorn is wrong,
   r/webdev, r/Teachers, school parent groups and small-business forums are
   right. Someone asking "how do I shrink these for the website" is a customer.
4. **The Adobe comparison is your headline.** "Pay once, £9, works offline,
   nothing uploaded" is a real, specific, checkable claim. Lead with it.

### Rules that keep you out of trouble

- **Never say "Photoshop" in a way that implies a connection to Adobe.**
  "Instead of Photoshop" is fine and true. Adobe's names are trademarks — you
  may compare, you may not imply endorsement or use their logos.
- **Don't claim it does things it doesn't.** The README's "what it is not"
  section is a feature. Overclaiming produces refunds and angry reviews, which
  cost far more than the sale was worth.
- **Offer refunds without arguing.** A refund costs £9. A chargeback costs the
  fee plus Gumroad's patience.
- **Keep collecting no personal data.** Toolsmith currently collects none,
  which is its best legal feature. Adding accounts or analytics on a tool used
  by children drags you into COPPA and the UK Children's Code. Don't.

### If it doesn't sell

That is the normal outcome, and it isn't wasted. You will have shipped a real,
working, six-tool application that runs offline, does honest pixel maths and
that people can use for free. That is a genuine portfolio piece and a genuine
public good. Leave it up, keep the free tier, and build the next thing.
