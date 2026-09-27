/*
 * Lays the pages out for folding. They are written in reading order; on an
 * eight-page sheet the fold wants them, top row left to right, 5 4 3 2
 * upside down, then 6 7 8 1 (8 the back, 1 the front). Every eight pages
 * make another sheet. The page numbers go on here, and the contents page
 * (an <ol class="contents">) is made from the pages' headings.
 *
 * A page opened as guide.html#check marks each page that runs over (`make check`).
 */
addEventListener("DOMContentLoaded", () => {
	const ORDER = [5, 4, 3, 2, 6, 7, 8, 1];
	const pages = [...document.querySelectorAll("section.page")];
	const body = document.body;
	const contents = document.querySelector("ol.contents");

	pages.forEach((p, i) => {
		const n = i + 1, folio = document.createElement("span");

		folio.className = "folio " + (n % 2 ? "right" : "left");
		folio.textContent = n;
		if (!p.classList.contains("nofolio"))
			p.appendChild(folio);
		p.dataset.n = n;
	});

	if (contents) {
		pages.forEach((p, i) => {
			if (p.dataset.part) {
				const li = document.createElement("li");

				li.className = "part";
				li.textContent = p.dataset.part;
				contents.appendChild(li);
			}
			const h = p.querySelector("h2");
			if (!h || p.classList.contains("cover") || p.classList.contains("toc"))
				return;
			const li = document.createElement("li"), t = document.createElement("span"),
			      n = document.createElement("span");

			t.textContent = h.dataset.short || h.textContent;
			n.className = "n";
			n.textContent = i + 1;
			li.append(t, n);
			contents.appendChild(li);
		});
	}

	for (let s = 0; s * 8 < pages.length; s++) {
		const sheet = document.createElement("div"), cut = document.createElement("span");

		sheet.className = "sheet";
		cut.className = "scissors";
		cut.textContent = "✂︎";
		sheet.append(cut);
		ORDER.forEach((n, i) => {
			const p = pages[s * 8 + n - 1] || document.createElement("section");

			p.classList.add("page", i < 4 ? "top" : "bottom");
			sheet.appendChild(p);
		});
		body.appendChild(sheet);
	}

	if (location.hash == "#check") {
		for (const p of pages) {
			const over = p.scrollHeight - p.clientHeight;

			p.dataset.check = (over > 0 ? "OVER " : "fits ") + over + "px p" + p.dataset.n;
		}
	}
});
