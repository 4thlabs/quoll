// @vitest-environment node

import { readFileSync } from "node:fs";

import { expect, it } from "vitest";

// The TypeScript guide, whose blocks of code are excerpts of the notes example.
const guide = new URL("../../../../../docs/guides/typescript.md", import.meta.url);
const example = new URL("../../../", import.meta.url);

type Block = {
  language: string;
  code: string;
  // The heading of the section it is in.
  section: string;
  // The file of the example its caption names, from the example's directory: the line before it links to it, or
  // names it, `examples/notes/web/vite.config.ts`.
  file: string | undefined;
};

function blocks(markdown: string): Block[] {
  const lines = markdown.split("\n");
  const found: Block[] = [];
  let section = "";
  let caption = "";

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];

    if (line.startsWith("## ")) {
      section = line.slice(3);
    }

    const fence = /^```(\S*)$/.exec(line);

    if (!fence) {
      if (line.trim() !== "") {
        caption = line;
      }

      continue;
    }

    const start = i + 1;

    do {
      i++;
    } while (i < lines.length && lines[i] !== "```");

    const file = /examples\/notes\/([^)`#\s]+)/.exec(caption)?.[1];

    found.push({ language: fence[1], code: lines.slice(start, i).join("\n"), section, file });
    caption = "";
  }

  return found;
}

const all = blocks(readFileSync(guide, "utf8"));

it("finds the guide's blocks", () => {
  expect(all.filter((block) => block.file !== undefined).length).toBeGreaterThan(5);
});

// The Next.js recipe is of another app, checked by hand.
it("names the file of each TypeScript block, but the Next.js recipe's", () => {
  const unnamed = all.filter(
    (block) => block.language === "typescript" && block.file === undefined && !block.section.startsWith("Next.js"),
  );

  expect(unnamed.map((block) => block.code.split("\n")[0])).toEqual([]);
});

it.each(all.filter((block) => block.file !== undefined).map((block) => [block.file, block] as const))(
  "has its excerpt of %s in that file",
  (file, block) => {
    expect(readFileSync(new URL(file!, example), "utf8")).toContain(block.code);
  },
);
