//! The order of what a builder of a client or of a server is given: the helpers of quoll in the order they work in,
//! anything else where it was given.

/// The items, each with a rank put in one of the places the ranked items hold, sorted by rank, and those of one rank
/// in the order they came; an item with no rank keeps its place.
pub(crate) fn arrange<R: Ord + Copy, T>(items: Vec<(Option<R>, T)>) -> Vec<T> {
    let places: Vec<usize> = items
        .iter()
        .enumerate()
        .filter_map(|(index, (rank, _))| rank.map(|_| index))
        .collect();
    let mut sorted = places.clone();

    // A stable sort, so that the items of one rank keep their order.
    sorted.sort_by_key(|&index| items[index].0);

    let mut taken_from: Vec<usize> = (0..items.len()).collect();

    for (&place, &index) in places.iter().zip(&sorted) {
        taken_from[place] = index;
    }

    let mut items: Vec<Option<T>> = items.into_iter().map(|(_, item)| Some(item)).collect();

    taken_from
        .into_iter()
        .map(|index| {
            items[index]
                .take()
                .expect("each item is taken from one place only")
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_ranked_items_are_sorted_into_their_places_and_the_others_stay() {
        let items = vec![
            (Some(3), "retry"),
            (None, "mine"),
            (Some(1), "request id"),
            (Some(3), "retry again"),
            (None, "mine again"),
            (Some(0), "trace"),
        ];

        assert_eq!(
            arrange(items),
            [
                "trace",
                "mine",
                "request id",
                "retry",
                "mine again",
                "retry again"
            ]
        );
    }

    #[test]
    fn items_in_order_stay_in_order() {
        let items = vec![(Some(0), 'a'), (None, 'b'), (Some(1), 'c'), (Some(1), 'd')];

        assert_eq!(arrange(items), ['a', 'b', 'c', 'd']);
        assert!(arrange::<u8, char>(Vec::new()).is_empty());
    }
}
