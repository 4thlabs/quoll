# Dashboard: a list, a detail and a chart

A screen of an application, over a small products API: a list to filter at `/products`, and the page of a product at
`/products/{id}`, its detail to edit and its sales in a [chart.js](https://www.chartjs.org/) chart, a JavaScript widget
of the page.

```
dashboard/
├── api/        # the contract: product.hpp, products_api.hpp
├── server/     # serves it over HTTP: service.hpp handles each route, the products in memory
├── web/        # the page, in C++ compiled to WASM, and the chart's module
└── tests/      # the service through a REST client, and the page on memory_renderer
```

| Endpoint | Request | Response |
|---|---|---|
| `products::list` | `GET /api/products` | `200`, the products |
| `products::update` | `PUT /api/products/{id}`, `{"name": "...", "price": 1.5, "stock": 3}` | `200`, the product; `422` problem for an empty name or a negative figure, `404` for no product |
| `products::sales` | `GET /api/products/{id}/sales` | `200`, `{"months": [...], "units": [...]}`; `404` for no product |

## The page

| File | What it holds |
|---|---|
| `web/CMakeLists.txt`, `vite.config.js`, `package.json`, `index.html`, `dev.sh` | the build and the development server: all that is at the root, the code is in `web/src/` |
| `web/src/main.cpp` | `mount(app_shell, ...)`, in `#app`, its addresses through the browser's history |
| `web/src/main.js`, `style.css` | the page's script and its styles, Tailwind and daisyUI |
| `web/src/routes.hpp` | the routes, `/products` and `/products/{id}`, a redirect from `/` and a page for an unknown address, and the app's options |
| `web/src/components/` | a header and a source for each component: the header its props and `extern component<props> const name;`, the source its view, `constexpr component<props> name = ...`, so that a change of one compiles again only it and what places it |
| `web/src/components/app_shell` | the app's root, the page of the address in its `outlet()` |
| `web/src/components/products_layout` | the layout of `/products`: the heading, the list, and the page of the address in its `outlet()` |
| `web/src/components/product_list` | the filter, a `use_state` of its own, and the rows of the catalog's products it matches |
| `web/src/components/product_row` | one product, a `link_to` its page |
| `web/src/components/choose_product`, `missing_page` | the page of `/products` when none is chosen, and the one of an unknown address |
| `web/src/components/product_page` | the page of `/products/{id}`: the product the catalog holds, or why it is not shown |
| `web/src/components/product_view` | a product's detail, and its sales, loaded by a `use_effect` on its `id` prop into a `use_async_state`: the sales, whether they are loading and why they failed |
| `web/src/components/product_detail` | the product shown, edited through a `use_form` of its props: each field bound to one of the product's, what is typed kept until saved, and kept when typed while a save is on its way; its save, a `use_action` of the catalog's save run with the form, read as a state for whether it waits and why it failed |
| `web/src/components/sales_chart` | the `line_chart` widget, and the month of the point clicked |
| `web/src/contexts/catalog.hpp` | the products, a context the layout, the list, the product's page and its detail share: loaded by an `effect` on no state, saved by `save`, which the detail's action runs |
| `web/src/ui/labelled.hpp`, `error_text.hpp` | a field of the form under its name, and a failure told to the user: functions of the page's own, base components to come |
| `web/src/widgets/line_chart.js` | the widget's module: `mount`, `update` and `unmount` over chart.js, a click emitting `point_selected` |
| `web/src/client/products_client.hpp`, `.cpp` | the calls to the server, through a `rest::client` of the contract, in a source of their own |

The components move to their headers alone, in forms S and D, as [`docs/ui.md`](../../docs/ui.md#a-component) writes
them, with [#200](https://github.com/4thlabs/quoll/issues/200).

A product is an address: a click on a row goes there, the browser's back button comes back, and a link to it opens it.
The product's page shows a view of the product keyed by it: another product is another view, with nothing typed and
its own sales, loading from the start, the load of the one before stopped. A detail saves its product once at a time,
through the catalog, so that a save goes on as its detail is left; two details of one product, as one left and the one
come back to, may still send theirs together. A field changed while the save is on its way keeps what it holds once
it is answered.

## Run it

As [the notes page](../notes/web/README.md#run-it), with `samples/dashboard/web/dev.sh`: it builds and starts the
products server on port 8080, then Vite on port 5173. A change of a literal shows in some milliseconds; anything else
takes a rebuild of about 2.9 s on a 4-core i5, from a save to the page shown again. The chart's view is in a source of
its own, so that the widget's JSON does not compile again with the rest of the page.

```bash
cmake -B build && cmake --build build -j
./build/samples/dashboard/quoll_dashboard_server 8080 &
curl localhost:8080/api/products/1/sales
```

`ctest --test-dir build -R quoll` runs its tests with the others: `tests/service_test.cpp` calls the service through a
REST client over a router of the same program, and `tests/dashboard_page_test.cpp` runs the page natively, the calls
to the server defined in the test over products in memory, the chart logged by `memory_renderer`.

## What the screen lacked

Written with what Quoll has, step 6 of [the plan](../../docs/PLAN.md#step-6-the-ui-dsl-complete). It needed nothing that
JavaScript widgets do not give: chart.js takes its data as props, and a click comes back as an event, with no handle on
the widget and no WebSocket. What took code of its own, for base components to give:

- **An input showing a value it is given.** `bind()` holds a `state<std::string>`; a prop first went in through a
  decorator of the page's own, which `value()` now gives, and what is typed through `on_input`.
- **A labelled field, a number field.** `labelled()` is a function of the page's own, a base component to come. The
  price and the stock first took parsing and formatting by hand, which `bind()` of a `form_state` now does.
- **States that start from the props.** A struct component's `mounted()` had no props, so the detail first kept what
  is typed as `state<std::optional<std::string>>`, nothing until a field changed; `use_form` now starts from the
  product the props give. It is keyed by the product so that another starts empty.
- **Requests that overlap.** The sales of a product chosen before the last may answer, or fail, after it; two saves of a
  product may be answered out of order, or sent by a double click. The page first kept counters for these by hand, in
  which reviews kept finding bugs; `quoll::ui` now gives `effect`, React's `useEffect` on the product chosen, whose run
  stops the one before, and `action`, with which the detail saves its product once at a time; the view reads
  whether each is under way and why it failed, the effect's in its target, the action's in the action itself. The products are a context, the `catalog`, which the
  page, the list and the detail share, and which does the save: a detail left stops waiting for it, and the save goes
  on, the list showing what the server saved. The detail first compared what each field held with what was saved
  before forgetting it: the `form_state` now forgets a field that holds what its props give again.
- **What a child tells, one task later.** A callback runs as a task of the component that wrote it: a field's change
  first reached the detail through one, after the event that made it, so that a save dispatched in the same turn, as a
  test does, read the value before it. Bound to the form, a field now changes it in its event's own handler.
