# 0184 — Dashboard navigation: one vocabulary, and a rule for which surface answers what

## Status

Accepted; one decision in it (menus are one level with headings) superseded by [ADR-0185](0185-menu-submenus-and-tabs-as-addresses.md), which makes the branch names real hover submenus. The vocabulary rule and everything else here stands.

## Context

The dashboard has two navigation surfaces: a left tree and a header menu bar. ADR-0138 reorganized the tree and moved every create action into one shared modal, reached from a single `+ Create` dropdown. Later work split that dropdown into topical menus. Neither change ever asked whether the two surfaces agreed with each other, and by the end of this sprint they did not.

The menu bar's topics were `Create`, `Software`, `User / Group`, `Network Services`, `Hardware`, `Host`. The tree's top level was Containers, Networks, Disks, Services, System. Two of the six menu topics — "Hardware" and "User / Group" — were not a word anywhere in the tree, and "Network Services" named a grouping the tree called "Services". So the same subject had two different names depending on which half of the screen you happened to be looking at, and knowing where a thing lived in one surface told you nothing about where to create it in the other.

Three smaller symptoms of the same drift, all found while fixing it: an `Assign Disk Role` action filed under "Hardware" while disks themselves were a top-level tree branch; `Bootstrap Build Image` filed under "Hardware" when it builds an image; and four `+ Create > …` paths in the operator guide that no longer existed at all.

This is the One Source of Truth maxim applied to naming rather than to state. Two names for one thing is the same defect as two records for one fact.

## Decision

**The two surfaces share one vocabulary and divide one job.**

The menu bar's topics ARE the tree's top-level names, in the tree's order: Containers, Networks, Disks, Services, System. Where a menu covers a branch that has children (Services, System), it carries sub-headings — one per child of that branch, in that branch's own order.

The division of labour is one sentence: **the tree is where you go to look at something; the menu bar is where you go to make one.** From which follows the placement rule for every future create action: *whatever branch a thing lives under is the menu that creates it.* A volume is created from Disks because volumes appear in the tree under the device holding them. An LDAP user is created from Services > LDAP because that is the leaf it belongs to. No judgement call is left over, which is what makes it a rule rather than a convention.

**The alignment is physical as well as lexical.** The logo cell is exactly as wide as the tree column — read from the same CSS variable the layout grid uses, so it tracks the tree while an operator drags it — and exactly as tall as the header, so its right border continues unbroken into the tree panel's own. The first topic's label sits at the same left inset as the page content directly below it. The intent is that the page reads as two columns all the way down, rather than as a header that happens to sit above a tree.

**Double-clicking a tree row opens or closes it.** The chevron is a ~14px target, and was the only way to expand a group without also navigating into it. The handler is on the label, not the whole row, so the chevron keeps its own single-click meaning; the first click of the double still navigates, deliberately.

## Alternatives considered

**Leave the menu bar's own topics and just rename the mismatched two.** Cheaper, and it would have closed the literal naming gap. Rejected because it leaves the harder half unanswered: with no rule for which menu owns a new action, the next few features re-open the drift one action at a time, exactly as this one accumulated.

**Fold creation into the tree — a "+" on each branch, no menu bar.** Genuinely tempting, and it makes the naming problem impossible by construction. Rejected because it puts destructive-adjacent affordances into the surface used for ordinary navigation, and because the tree is already dense with live per-resource state (status tints, right-click actions); a create affordance on every branch competes with the thing the tree is actually good at.

**Mirror the tree exactly, three levels deep, in the menu bar.** Rejected as a re-run of the mistake ADR-0138's successors kept correcting in the tree itself: navigating a hierarchy to find out which of several pages a thing is on. The menus are one level with headings, which is enough for a list of at most eleven actions.

## Consequences

Adding a create action now has an answer rather than a discussion: file it under the branch its resource lives in. Adding a top-level tree branch obliges a matching menu topic, and renaming one obliges renaming the other — the coupling is deliberate, and it is the property that keeps them from drifting apart again.

The dashboard's own guide gains a section stating the rule, so the next person to add a form has somewhere to read it. The alignment work also means the tree's resize handle now drives a CSS variable rather than the grid directly; anything else that wants to line up with the tree column reads the same variable.

Below 720px the tree is a full-width band rather than a column, so there is no edge left to align to: the logo shrinks to its content, its border comes off, and the topics wrap.

Client-side only. No API, CLI or daemon change — every action already had an endpoint, which is why this could be a pure renaming and re-filing exercise in the first place.
