const PREPEND = "http://localhost:5500/";

const observer = new MutationObserver((mutationList, observer) => {
    for (const m of mutationList) {
        for (let n of m.addedNodes) {
            if (n.tagName == "IMG") {
                n.src = PREPEND+n.src;
            }
        }
    }
});

observer.observe(document, { childList: true, subtree: true });
