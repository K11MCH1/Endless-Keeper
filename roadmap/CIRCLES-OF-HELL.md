# Circles of Hell

> [!NOTE]
> This is a planned feature. It is not in the game yet, and details may change before it arrives.

## What it is for

The endless world gets more dangerous the farther you go from a dungeon, but nothing tells you so until
something kills your creatures. The Circles of Hell make that danger visible and worth chasing: the land
around every dungeon is cut into circles, each with stronger residents and better loot than the one before.
Each one farther out is worse than the last. The keeper sits in the middle, and the suffering is whatever
lives outside.

## The nine circles

A circle is 250 slabs wide. Circles are measured from the **nearest dungeon start**, not from the centre of
the world, so every keeper has their own set and always begins in the First Circle.

| Circle | Distance from the nearest start | Residents |
|---|---|---|
| First | 0 to 250 | 50% of the nearest keeper's best creature level |
| Second | 250 to 500 | 75% |
| Third | 500 to 750 | 100% |
| Fourth | 750 to 1,000 | 125% |
| Fifth | 1,000 to 1,250 | Level 30 |
| Sixth | 1,250 to 1,500 | Level 45 |
| Seventh | 1,500 to 1,750 | Level 60 |
| Eighth | 1,750 to 2,000 | Level 80 |
| Ninth | Beyond 2,000 | Level 100 |

Every circle has a name of its own, and a few words to go with it. They are not written here. You learn them
by getting there.

Loot follows the same order: each circle drops better things than the one before it.

Dungeon starts are placed at least 1,000 slabs apart, so the land between neighbours never gets past the
Fourth Circle. The Fifth and beyond are only found **outwards**, past the outermost dungeons, where nobody
lives.

## Crossing into a circle

A keeper breaches a circle when their imps dig through its line. Moving the view across a line does nothing.
When the imps break through, the game says so:

1. The circle's name fades in at the top of the screen.
2. Its line fades in just below the name.
3. Both stay for three seconds, then fade out together.

A sound of its own may play with it.

This happens only once for each circle: the first time that keeper's imps reach it.

## How enemies scale

It depends on who starts the fight.

| Enemy | Scales with |
|---|---|
| Night raids and hero waves that come to your dungeon | Your own best creature level, as now. Circles don't change them. |
| Outposts, wild creatures and loot you travel to | The circle they stand in. |
| Enemy keepers | The circle they stand in, and they grow slowly while left alone. |

Defending your home stays fair for a new keeper next to an old one. The circles only decide how hard it is
to go out.

### Keeping Up Appearances: the first four circles

In the first four circles the locals do their best to stay a match for the keeper next door. Their level is a
share of the **nearest keeper's best creature level**: half of it in the First Circle, rising to a quarter
above it in the Fourth. As that keeper grows, so do their surroundings, a little each day, and never below
the levels the world has today.

A keeper with level 60 creatures therefore always has a proper fight within reach, and a new keeper has
gentle surroundings to grow in before wandering into anyone else's.

### The fixed circles: the Fifth and beyond

From the Fifth Circle on, levels are fixed and don't care who is asking. This is the deep: the long trip out,
the hardest fights and the best loot, ending at level 100 in the Ninth.

## When a new keeper arrives

A new dungeon always appears on land that was far from everyone, which means land that was deep for somebody.

- **The Welcome Mat.** Within 250 slabs of the new start, whatever already lived there is brought down to
  First Circle strength. The locals are told to go easy.
- **Old Grudges.** Farther out, what already exists stays as strong as it was, and only what appears later
  follows the new circles. The few strong outposts left over were here first and remember it. They are worth
  coming back for.

## On the map

- The circles are drawn as faint lines on the full-screen map. A circle's name appears on the map once you
  have breached it.
- The lines between two keepers meet where their circles do, so the map shows at a glance whose surroundings
  you are standing in.

## Still being decided

- The exact shares and levels in the table above.
- Whether an old keeper's surroundings should be softened for a much newer keeper who walks into them.
- How circles work around a co-op dungeon: most likely from the party's best creature level.
